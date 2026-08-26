#include "generation.h"

#include "json.h"
#include "protocol-codec.h"
#include "response-types.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace llama_responses {
namespace {

class scripted_generation_session final : public generation_session {
  public:
    explicit scripted_generation_session(std::vector<generation_update> script) : script(std::move(script)) {}

    std::optional<generation_update> next() override {
        if (terminal_emitted) {
            return std::nullopt;
        }
        if (cancelled.load()) {
            terminal_emitted = true;
            cursor           = script.size();
            return generation_cancelled{};
        }
        if (cursor == script.size()) {
            return std::nullopt;
        }

        generation_update result = script[cursor++];
        terminal_emitted         = generation_update_is_terminal(result);
        return result;
    }

    void request_cancel() noexcept override { cancelled.store(true); }

    bool cancel_requested() const noexcept override { return cancelled.load(); }

  private:
    std::vector<generation_update> script;
    std::size_t                    cursor = 0;
    std::atomic_bool               cancelled{ false };
    bool                           terminal_emitted = false;
};

common_json reasoning_part(const std::string & text) {
    return {
        { "type", "summary_text" },
        { "text", text           },
    };
}

common_json reasoning_content(const std::string & text) {
    return {
        { "type", "reasoning_text" },
        { "text", text             },
    };
}

common_json text_part(const std::string & text) {
    return {
        { "type",        "output_text"        },
        { "annotations", common_json::array() },
        { "logprobs",    common_json::array() },
        { "text",        text                 },
    };
}

common_json parse_tool_arguments(const std::string & arguments) {
    if (arguments.empty()) {
        return common_json::object();
    }
    try {
        return common_json::parse(arguments);
    } catch (const std::exception &) {
        return common_json::object();
    }
}

common_json parse_tool_search_arguments(const std::string & arguments, bool require_complete) {
    // Client tool-search has no public argument-delta event. Keep the
    // in-progress snapshot aligned with output_item.added and reveal the
    // buffered object only when the item closes.
    if (!require_complete || arguments.empty()) {
        return common_json::object();
    }
    try {
        common_json parsed = common_json::parse(arguments);
        if (!parsed.is_object()) {
            throw std::invalid_argument("generated client tool-search arguments must be a JSON object");
        }
        return parsed;
    } catch (const std::invalid_argument &) {
        throw;
    } catch (const std::exception &) {
        throw std::invalid_argument("generated client tool-search arguments must be a valid JSON object");
    }
}

// Keep this projection aligned with
// tools/server/server-task.cpp::build_local_shell_action while the
// transitional Chat-shaped input/output parser remains in use.
common_json local_shell_action(const std::string & raw_arguments) {
    const common_json arguments = parse_tool_arguments(raw_arguments);
    if (arguments.contains("action") && arguments.at("action").is_object()) {
        return arguments.at("action");
    }

    common_json action = {
        { "type",    "exec"               },
        { "command", common_json::array() },
    };
    if (arguments.contains("command")) {
        const common_json & command = arguments.at("command");
        if (command.is_array()) {
            action["command"] = command;
        } else if (command.is_string()) {
            action["command"] = common_json::array({ "bash", "-lc", command.get<std::string>() });
        }
    } else if (arguments.contains("cmd") && arguments.at("cmd").is_string()) {
        action["command"] = common_json::array({ "bash", "-lc", arguments.at("cmd").get<std::string>() });
    } else if (!raw_arguments.empty()) {
        action["command"] = common_json::array({ "bash", "-lc", raw_arguments });
    }

    for (const char * key : { "timeout_ms", "working_directory", "env", "user" }) {
        if (arguments.contains(key)) {
            action[key] = arguments.at(key);
        }
    }
    return action;
}

response_event_type terminal_event_type(response_status status) {
    switch (status) {
        case response_status::completed:
            return response_event_type::completed;
        case response_status::incomplete:
            return response_event_type::incomplete;
        case response_status::failed:
            return response_event_type::failed;
        case response_status::cancelled:
            return response_event_type::cancelled;
        case response_status::queued:
        case response_status::in_progress:
            throw std::invalid_argument("terminal event requires a terminal response status");
    }
    throw std::invalid_argument("unknown response status");
}

std::string normalized_namespace(std::string value) {
    if (value.empty()) {
        return "generation";
    }
    return value;
}

}  // namespace

bool generation_update_is_terminal(const generation_update & update) noexcept {
    return std::holds_alternative<generation_completed>(update) ||
           std::holds_alternative<generation_incomplete>(update) || std::holds_alternative<generation_failed>(update) ||
           std::holds_alternative<generation_cancelled>(update);
}

scripted_generation_port::scripted_generation_port(std::vector<generation_update> script) : script(std::move(script)) {}

std::unique_ptr<generation_session> scripted_generation_port::start(const generation_request & /*request*/) {
    return std::make_unique<scripted_generation_session>(script);
}

counter_generation_id_source::counter_generation_id_source(std::string namespace_value) :
    namespace_value(normalized_namespace(std::move(namespace_value))) {}

std::string counter_generation_id_source::next_value(const char * prefix, std::uint64_t & counter) {
    std::lock_guard<std::mutex> lock(mutex);
    return std::string(prefix) + namespace_value + '_' + std::to_string(counter++);
}

response_id counter_generation_id_source::next_response_id() {
    return response_id(next_value("resp_", response_counter));
}

item_id counter_generation_id_source::next_item_id(generation_item_kind kind) {
    switch (kind) {
        case generation_item_kind::reasoning:
            return item_id(next_value("rs_", reasoning_counter));
        case generation_item_kind::message:
            return item_id(next_value("msg_", message_counter));
        case generation_item_kind::function_call:
            return item_id(next_value("fc_", function_counter));
        case generation_item_kind::custom_tool_call:
            return item_id(next_value("ctc_", custom_counter));
        case generation_item_kind::local_shell_call:
            return item_id(next_value("lsc_", local_shell_counter));
        case generation_item_kind::tool_search_call:
            return item_id(next_value("tsc_", tool_search_counter));
    }
    throw std::invalid_argument("unknown generation item kind");
}

call_id counter_generation_id_source::next_call_id(const std::string & upstream_call_id) {
    if (upstream_call_id.rfind("call_", 0) == 0) {
        return call_id(upstream_call_id);
    }
    if (!upstream_call_id.empty()) {
        return call_id("call_" + upstream_call_id);
    }
    return call_id(next_value("call_", call_counter));
}

common_json render_generation_event(const response_event & event) {
    common_json result        = event.data.is_object() ? event.data : common_json::object();
    result["type"]            = response_event_type_name(event.type);
    result["sequence_number"] = event.sequence_number;
    return result;
}

class native_response_state_machine::impl {
  public:
    impl(generation_response_context context, generation_id_source & ids) : ids(ids) {
        if (!context.request.is_object()) {
            throw std::invalid_argument("generation response request must be an object");
        }
        if (!context.input_items.is_array()) {
            throw std::invalid_argument("generation response input_items must be an array");
        }
        if (!context.wire_snapshot.is_object()) {
            throw std::invalid_argument("generation response wire_snapshot must be an object");
        }

        response.id                = ids.next_response_id();
        response.status            = response_status::in_progress;
        response.created_at        = context.created_at;
        response.model             = std::move(context.model);
        response.request           = std::move(context.request);
        response.input_items       = std::move(context.input_items);
        response.wire_snapshot     = std::move(context.wire_snapshot);
        response.previous_response = std::move(context.previous_response);
        if (response.id.empty()) {
            throw std::invalid_argument("generation id source returned an empty response id");
        }
        if (!response.previous_response && response.request.contains("previous_response_id") &&
            response.request.at("previous_response_id").is_string()) {
            const std::string previous = response.request.at("previous_response_id").get<std::string>();
            if (!previous.empty()) {
                response.previous_response = response_id(previous);
            }
        }
        if (response.request.contains("metadata") && response.request.at("metadata").is_object()) {
            response.metadata = response.request.at("metadata");
        }
    }

    std::vector<response_event> start() {
        ensure_started();
        return take_pending_events();
    }

    std::vector<response_event> apply(const generation_update & update) {
        if (is_terminal) {
            throw std::logic_error("generation update arrived after the terminal update");
        }

        ensure_started();
        std::visit([this](const auto & value) { apply_one(value); }, update);
        return take_pending_events();
    }

    const response_state & active_state() const noexcept { return response; }

    response_state materialized_state() const {
        response_state result = response;
        if (reasoning_item) {
            materialize_reasoning_item(result.output.at(*reasoning_item));
        }
        if (message_item) {
            materialize_message_item(result.output.at(*message_item));
        }
        for (const auto & entry : tools) {
            const tool_state & tool = entry.second;
            if (!tool.done) {
                materialize_tool_item(tool, result.output.at(tool.response_item_index), false);
            }
        }
        return result;
    }

    common_json snapshot() const { return render_response(materialized_state()); }

    std::vector<response_event> take_pending_events() noexcept {
        std::vector<response_event> result;
        result.swap(pending_events);
        return result;
    }

    bool terminal() const noexcept { return is_terminal; }

  private:
    struct tool_state {
        generation_tool_kind kind                = generation_tool_kind::function;
        std::size_t          output_index        = 0;
        std::size_t          response_item_index = 0;
        item_id              item;
        call_id              call;
        std::string          name;
        std::string          namespace_name;
        std::string          value;
        bool                 done = false;
    };

    generation_id_source &            ids;
    response_state                    response;
    std::vector<response_event>       pending_events;
    std::optional<std::size_t>        reasoning_item;
    std::optional<std::size_t>        message_item;
    std::optional<std::size_t>        open_reasoning;
    std::optional<std::size_t>        open_message;
    std::string                       reasoning_text;
    std::string                       message_text;
    std::map<std::size_t, tool_state> tools;
    bool                              started     = false;
    bool                              is_terminal = false;

    response_event & emit(response_event_type        type,
                          common_json                data,
                          std::optional<item_id>     item          = std::nullopt,
                          std::optional<std::size_t> output_index  = std::nullopt,
                          std::optional<std::size_t> content_index = std::nullopt) {
        response_event event;
        event.type            = type;
        event.sequence_number = response.next_sequence_number;
        event.response        = response.id;
        event.item            = std::move(item);
        event.output_index    = output_index;
        event.content_index   = content_index;
        event.data            = std::move(data);
        pending_events.push_back(std::move(event));
        ++response.next_sequence_number;
        return pending_events.back();
    }

    void emit_response(response_event_type type) {
        emit(type, {
                       { "response", snapshot() }
        });
    }

    void ensure_started() {
        if (started) {
            return;
        }
        started = true;
        emit_response(response_event_type::created);
        emit_response(response_event_type::in_progress);
    }

    bool item_id_exists(const item_id & candidate) const {
        return std::any_of(response.output.begin(), response.output.end(),
                           [&candidate](const response_output_item & item) { return item.id == candidate; });
    }

    std::size_t append_item(response_output_item item) {
        if (item.id.empty()) {
            throw std::invalid_argument("generation id source returned an empty item id");
        }
        if (item_id_exists(item.id)) {
            throw std::invalid_argument("generation id source returned a duplicate item id");
        }
        response.output.push_back(std::move(item));
        return response.output.size() - 1;
    }

    response_output_item & output_item(std::size_t index) { return response.output.at(index); }

    void open_reasoning_item() {
        if (open_reasoning) {
            return;
        }
        response_output_item item;
        item.id    = ids.next_item_id(generation_item_kind::reasoning);
        item.type  = "reasoning";
        item.value = {
            { "id",                item.id.str()        },
            { "type",              item.type            },
            { "status",            "in_progress"        },
            { "summary",           common_json::array() },
            { "content",           common_json::array() },
            { "encrypted_content", ""                   },
        };
        open_reasoning                 = append_item(std::move(item));
        reasoning_item                 = open_reasoning;
        const std::size_t output_index = *open_reasoning;
        const auto &      stored       = output_item(output_index);
        emit(response_event_type::output_item_added,
             {
                 { "output_index", output_index               },
                 { "item",         render_output_item(stored) },
        },
             stored.id, output_index);
        emit(response_event_type::reasoning_summary_part_added,
             {
                 { "output_index",  output_index       },
                 { "summary_index", 0                  },
                 { "item_id",       stored.id.str()    },
                 { "part",          reasoning_part("") },
        },
             stored.id, output_index);
    }

    void materialize_reasoning_item(response_output_item & item) const {
        item.value["summary"] = common_json::array({ reasoning_part(reasoning_text) });
        item.value["content"] = common_json::array({ reasoning_content(reasoning_text) });
    }

    void close_reasoning_item(const char * status) {
        if (!open_reasoning) {
            return;
        }
        const std::size_t      output_index = *open_reasoning;
        response_output_item & item         = output_item(output_index);
        item.value["status"]                = status;
        materialize_reasoning_item(item);
        emit(response_event_type::reasoning_summary_text_done,
             {
                 { "output_index",  output_index   },
                 { "summary_index", 0              },
                 { "item_id",       item.id.str()  },
                 { "text",          reasoning_text },
        },
             item.id, output_index);
        emit(response_event_type::reasoning_text_done,
             {
                 { "output_index",  output_index   },
                 { "content_index", 0              },
                 { "item_id",       item.id.str()  },
                 { "text",          reasoning_text },
        },
             item.id, output_index, 0);
        emit(response_event_type::reasoning_summary_part_done,
             {
                 { "output_index",  output_index                   },
                 { "summary_index", 0                              },
                 { "item_id",       item.id.str()                  },
                 { "part",          reasoning_part(reasoning_text) },
        },
             item.id, output_index);
        emit(response_event_type::output_item_done,
             {
                 { "output_index", output_index             },
                 { "item",         render_output_item(item) },
        },
             item.id, output_index);
        open_reasoning.reset();
    }

    void open_message_item() {
        if (open_message) {
            return;
        }
        close_reasoning_item("completed");
        response_output_item item;
        item.id    = ids.next_item_id(generation_item_kind::message);
        item.type  = "message";
        item.value = {
            { "id",      item.id.str()        },
            { "type",    item.type            },
            { "status",  "in_progress"        },
            { "role",    "assistant"          },
            { "phase",   "commentary"         },
            { "content", common_json::array() },
        };
        open_message                   = append_item(std::move(item));
        message_item                   = open_message;
        const std::size_t output_index = *open_message;
        const auto &      stored       = output_item(output_index);
        emit(response_event_type::output_item_added,
             {
                 { "output_index", output_index               },
                 { "item",         render_output_item(stored) },
        },
             stored.id, output_index);
        emit(response_event_type::content_part_added,
             {
                 { "output_index",  output_index    },
                 { "content_index", 0               },
                 { "item_id",       stored.id.str() },
                 { "part",          text_part("")   },
        },
             stored.id, output_index, 0);
    }

    void materialize_message_item(response_output_item & item) const {
        item.value["content"] = common_json::array({ text_part(message_text) });
    }

    void close_message_item(const char * status, bool has_tools) {
        if (!open_message) {
            return;
        }
        const std::size_t      output_index = *open_message;
        response_output_item & item         = output_item(output_index);
        item.value["status"]                = status;
        item.value["phase"]                 = has_tools ? "commentary" : "final_answer";
        materialize_message_item(item);
        const common_json part = text_part(message_text);
        emit(response_event_type::output_text_done,
             {
                 { "output_index",  output_index         },
                 { "content_index", 0                    },
                 { "item_id",       item.id.str()        },
                 { "text",          message_text         },
                 { "logprobs",      common_json::array() },
        },
             item.id, output_index, 0);
        emit(response_event_type::content_part_done,
             {
                 { "output_index",  output_index  },
                 { "content_index", 0             },
                 { "item_id",       item.id.str() },
                 { "part",          part          },
        },
             item.id, output_index, 0);
        emit(response_event_type::output_item_done,
             {
                 { "output_index", output_index             },
                 { "item",         render_output_item(item) },
        },
             item.id, output_index);
        open_message.reset();
    }

    void apply_one(const generation_started & /*update*/) {}

    void apply_one(const generation_progress & /*update*/) {
        // llama-server can report prompt-processing progress more than once.
        // Responses already emitted its single lifecycle in_progress event at
        // start; projecting runtime progress as another full response envelope
        // would repeatedly copy static input and the growing output prefix.
    }

    void apply_one(const generation_reasoning_delta & update) {
        if (update.delta.empty()) {
            return;
        }
        if (open_message || !tools.empty()) {
            throw std::logic_error("reasoning delta arrived after answer or tool output began");
        }
        open_reasoning_item();
        if (!open_reasoning) {
            throw std::logic_error("reasoning item was not opened");
        }
        const std::size_t output_index = open_reasoning.value();
        reasoning_text += update.delta;
        const response_output_item & item = output_item(output_index);
        emit(response_event_type::reasoning_summary_text_delta,
             {
                 { "output_index",  output_index  },
                 { "summary_index", 0             },
                 { "item_id",       item.id.str() },
                 { "delta",         update.delta  },
        },
             item.id, output_index);
        emit(response_event_type::reasoning_text_delta,
             {
                 { "output_index",  output_index  },
                 { "content_index", 0             },
                 { "item_id",       item.id.str() },
                 { "delta",         update.delta  },
        },
             item.id, output_index, 0);
    }

    void apply_one(const generation_text_delta & update) {
        if (update.delta.empty()) {
            return;
        }
        if (!tools.empty()) {
            throw std::logic_error("text delta arrived after tool output began");
        }
        open_message_item();
        if (!open_message) {
            throw std::logic_error("message item was not opened");
        }
        const std::size_t output_index = open_message.value();
        message_text += update.delta;
        const response_output_item & item = output_item(output_index);
        emit(response_event_type::output_text_delta,
             {
                 { "output_index",  output_index  },
                 { "content_index", 0             },
                 { "item_id",       item.id.str() },
                 { "delta",         update.delta  },
        },
             item.id, output_index, 0);
    }

    void apply_one(const generation_tool_call_started & update) {
        if (update.name.empty()) {
            throw std::invalid_argument("generation tool call name must not be empty");
        }
        if (tools.find(update.index) != tools.end()) {
            throw std::invalid_argument("generation tool call index was started more than once");
        }
        close_reasoning_item("completed");
        close_message_item("completed", true);

        tool_state tool;
        tool.kind                      = update.kind;
        tool.name                      = update.name;
        tool.namespace_name            = update.namespace_name;
        generation_item_kind item_kind = generation_item_kind::function_call;
        switch (update.kind) {
            case generation_tool_kind::function:
                item_kind = generation_item_kind::function_call;
                break;
            case generation_tool_kind::custom:
                item_kind = generation_item_kind::custom_tool_call;
                break;
            case generation_tool_kind::local_shell:
                item_kind = generation_item_kind::local_shell_call;
                break;
            case generation_tool_kind::client_tool_search:
                item_kind = generation_item_kind::tool_search_call;
                break;
        }
        tool.item = ids.next_item_id(item_kind);
        tool.call = ids.next_call_id(update.upstream_call_id);
        if (tool.call.empty()) {
            throw std::invalid_argument("generation id source returned an empty call id");
        }
        const bool duplicate_call = std::any_of(tools.begin(), tools.end(),
                                                [&tool](const auto & entry) { return entry.second.call == tool.call; });
        if (duplicate_call) {
            throw std::invalid_argument("generation id source returned a duplicate call id");
        }

        response_output_item item;
        item.id   = tool.item;
        item.call = tool.call;
        switch (update.kind) {
            case generation_tool_kind::function:
                item.type = "function_call";
                break;
            case generation_tool_kind::custom:
                item.type = "custom_tool_call";
                break;
            case generation_tool_kind::local_shell:
                item.type = "local_shell_call";
                break;
            case generation_tool_kind::client_tool_search:
                item.type = "tool_search_call";
                break;
        }
        item.value = {
            { "id",      item.id.str()   },
            { "call_id", tool.call.str() },
            { "type",    item.type       },
            { "status",  "in_progress"   },
        };
        if (update.kind == generation_tool_kind::local_shell) {
            item.value["action"] = local_shell_action("");
        } else if (update.kind == generation_tool_kind::client_tool_search) {
            item.value["execution"] = "client";
            item.value["arguments"] = common_json::object();
        } else {
            item.value["name"]                                                                = update.name;
            item.value[update.kind == generation_tool_kind::function ? "arguments" : "input"] = "";
            if (!update.namespace_name.empty()) {
                item.value["namespace"] = update.namespace_name;
            }
        }
        tool.response_item_index       = append_item(std::move(item));
        tool.output_index              = tool.response_item_index;
        const auto         inserted    = tools.emplace(update.index, std::move(tool));
        const tool_state & stored_tool = inserted.first->second;
        const auto &       stored_item = output_item(stored_tool.response_item_index);
        emit(response_event_type::output_item_added,
             {
                 { "output_index", stored_tool.output_index        },
                 { "item",         render_output_item(stored_item) },
        },
             stored_tool.item, stored_tool.output_index);
    }

    void apply_one(const generation_tool_call_delta & update) {
        const auto found = tools.find(update.index);
        if (found == tools.end()) {
            throw std::invalid_argument("generation tool call delta has no matching start update");
        }
        tool_state & tool = found->second;
        if (tool.done) {
            throw std::logic_error("generation tool call delta arrived after the item was closed");
        }
        if (update.delta.empty()) {
            return;
        }
        tool.value += update.delta;
        if (tool.kind == generation_tool_kind::function) {
            emit(response_event_type::function_call_arguments_delta,
                 {
                     { "output_index", tool.output_index },
                     { "item_id",      tool.item.str()   },
                     { "delta",        update.delta      },
            },
                 tool.item, tool.output_index);
            return;
        }
        if (tool.kind == generation_tool_kind::local_shell) {
            return;
        }
        if (tool.kind == generation_tool_kind::client_tool_search) {
            // OpenAI exposes client tool-search arguments as one JSON value
            // on the generic output-item lifecycle. There is no corresponding
            // public tool-search argument-delta event to manufacture here.
            return;
        }
        emit(response_event_type::custom_tool_call_input_delta,
             {
                 { "output_index", tool.output_index },
                 { "item_id",      tool.item.str()   },
                 { "delta",        update.delta      },
        },
             tool.item, tool.output_index);
    }

    void apply_one(const generation_message_reconciliation & update) {
        if (!reasoning_item && !update.reasoning.empty()) {
            if (!response.output.empty()) {
                throw std::logic_error("final reasoning appeared after another output item was opened");
            }
            open_reasoning_item();
        }
        reasoning_text = update.reasoning;

        if (!message_item && !update.text.empty()) {
            if (!tools.empty()) {
                throw std::logic_error("final message appeared after tool output began");
            }
            open_message_item();
        }
        message_text = update.text;

        for (const generation_tool_call_reconciliation & tool_update : update.tools) {
            const auto found = tools.find(tool_update.index);
            if (found == tools.end()) {
                throw std::invalid_argument("final tool value has no matching start update");
            }
            tool_state & tool = found->second;
            if (tool.done) {
                throw std::logic_error("final tool value arrived after the item was closed");
            }
            tool.value = tool_update.value;
        }
    }

    void apply_one(const generation_usage_update & update) { response.usage = update.usage; }

    std::vector<tool_state *> tools_in_output_order() {
        std::vector<tool_state *> ordered;
        ordered.reserve(tools.size());
        for (auto & entry : tools) {
            ordered.push_back(&entry.second);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const tool_state * lhs, const tool_state * rhs) { return lhs->output_index < rhs->output_index; });
        return ordered;
    }

    static void materialize_tool_item(const tool_state & tool, response_output_item & item, bool require_complete) {
        switch (tool.kind) {
            case generation_tool_kind::function:
                item.value["arguments"] = tool.value;
                break;
            case generation_tool_kind::custom:
                item.value["input"] = tool.value;
                break;
            case generation_tool_kind::local_shell:
                item.value["action"] = local_shell_action(tool.value);
                break;
            case generation_tool_kind::client_tool_search:
                item.value["arguments"] = parse_tool_search_arguments(tool.value, require_complete);
                break;
        }
    }

    void close_tool(tool_state & tool, const char * status) {
        if (tool.done) {
            return;
        }
        response_output_item & item = output_item(tool.response_item_index);
        item.value["status"]        = status;
        materialize_tool_item(tool, item, std::string(status) == "completed");
        if (tool.kind == generation_tool_kind::function) {
            emit(response_event_type::function_call_arguments_done,
                 {
                     { "output_index", tool.output_index },
                     { "item_id",      tool.item.str()   },
                     { "name",         tool.name         },
                     { "arguments",    tool.value        },
            },
                 tool.item, tool.output_index);
        } else if (tool.kind == generation_tool_kind::custom) {
            emit(response_event_type::custom_tool_call_input_done,
                 {
                     { "output_index", tool.output_index },
                     { "item_id",      tool.item.str()   },
                     { "input",        tool.value        },
            },
                 tool.item, tool.output_index);
        }
        emit(response_event_type::output_item_done,
             {
                 { "output_index", tool.output_index        },
                 { "item",         render_output_item(item) },
        },
             tool.item, tool.output_index);
        tool.done = true;
    }

    void close_outputs(const char * status) {
        close_reasoning_item(status);
        close_message_item(status, !tools.empty());
        for (tool_state * tool : tools_in_output_order()) {
            close_tool(*tool, status);
        }
    }

    void finish(response_status                       status,
                const std::optional<response_usage> & usage,
                const std::optional<response_error> & error,
                common_json                           incomplete_details,
                std::optional<std::uint64_t>          completed_at) {
        const char * item_status = status == response_status::completed ? "completed" : "incomplete";
        close_outputs(item_status);
        response.status             = status;
        response.error              = error;
        response.incomplete_details = std::move(incomplete_details);
        response.completed_at       = completed_at;
        if (usage) {
            response.usage = *usage;
            response.wire_snapshot.erase("usage");
        } else {
            response.wire_snapshot["usage"] = nullptr;
        }
        is_terminal = true;
        emit_response(terminal_event_type(status));
    }

    void apply_one(const generation_completed & update) {
        const std::uint64_t completed_at = update.completed_at == 0 ? response.created_at : update.completed_at;
        finish(response_status::completed, update.usage, std::nullopt, nullptr, completed_at);
    }

    void apply_one(const generation_incomplete & update) {
        common_json details = {
            { "reason", update.reason.empty() ? "max_output_tokens" : update.reason },
        };
        finish(response_status::incomplete, update.usage, std::nullopt, std::move(details), std::nullopt);
    }

    void apply_one(const generation_failed & update) {
        close_outputs("incomplete");
        response.error = update.error;
        emit(response_event_type::error,
             {
                 { "code",    update.error.code                                                                   },
                 { "message", update.error.message                                                                },
                 { "param",   update.error.param.empty() ? common_json(nullptr) : common_json(update.error.param) },
        });
        finish(response_status::failed, update.usage, update.error, nullptr, std::nullopt);
    }

    void apply_one(const generation_cancelled & update) {
        finish(response_status::cancelled, update.usage, std::nullopt, nullptr, std::nullopt);
    }
};

native_response_state_machine::native_response_state_machine(generation_response_context context,
                                                             generation_id_source &      ids) :
    implementation(std::make_unique<impl>(std::move(context), ids)) {}

native_response_state_machine::~native_response_state_machine() = default;

native_response_state_machine::native_response_state_machine(native_response_state_machine &&) noexcept = default;

native_response_state_machine & native_response_state_machine::operator=(native_response_state_machine &&) noexcept =
    default;

std::vector<response_event> native_response_state_machine::start() {
    return implementation->start();
}

std::vector<response_event> native_response_state_machine::apply(const generation_update & update) {
    return implementation->apply(update);
}

const response_state & native_response_state_machine::active_state() const noexcept {
    return implementation->active_state();
}

response_state native_response_state_machine::materialized_state() const {
    return implementation->materialized_state();
}

common_json native_response_state_machine::snapshot() const {
    return implementation->snapshot();
}

std::vector<response_event> native_response_state_machine::take_pending_events() noexcept {
    return implementation->take_pending_events();
}

bool native_response_state_machine::terminal() const noexcept {
    return implementation->terminal();
}

}  // namespace llama_responses
