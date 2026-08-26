#include "server-generation-adapter.h"

#include "chat.h"
#include "generation.h"
#include "json.h"
#include "response-store.h"
#include "response-types.h"
#include "server-generation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace llama_responses {
namespace {

response_usage response_usage_from_server(const server_generation_usage & usage) {
    response_usage result;
    result.input_tokens        = usage.input_tokens;
    result.cached_input_tokens = usage.cached_input_tokens;
    result.output_tokens       = usage.output_tokens;
    return result;
}

response_error response_error_from_server(const server_generation_error & error) {
    return {
        error.code,
        error.message,
        error.param,
    };
}

bool same_error(const std::optional<response_error> & lhs, const std::optional<response_error> & rhs) {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || (lhs->code == rhs->code && lhs->message == rhs->message && lhs->param == rhs->param);
}

bool same_usage(const response_usage & lhs, const response_usage & rhs) {
    return lhs.input_tokens == rhs.input_tokens && lhs.cached_input_tokens == rhs.cached_input_tokens &&
           lhs.output_tokens == rhs.output_tokens && lhs.reasoning_output_tokens == rhs.reasoning_output_tokens;
}

bool same_output(const std::vector<response_output_item> & lhs, const std::vector<response_output_item> & rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].id != rhs[index].id || lhs[index].call != rhs[index].call ||
            lhs[index].type != rhs[index].type || lhs[index].value != rhs[index].value) {
            return false;
        }
    }
    return true;
}

// An ancestor deletion may fold prompt-visible lineage into an active child.
// That deliberately increments the child's store revision, but it is not a
// competing generation write. Retry only when every generation-owned field is
// still exactly the snapshot this sink last committed.
bool same_generation_state(const response_state & lhs, const response_state & rhs) {
    return lhs.id == rhs.id && lhs.status == rhs.status && lhs.created_at == rhs.created_at &&
           lhs.completed_at == rhs.completed_at && lhs.next_sequence_number == rhs.next_sequence_number &&
           lhs.model == rhs.model && lhs.previous_response == rhs.previous_response && lhs.request == rhs.request &&
           lhs.metadata == rhs.metadata && lhs.input_items == rhs.input_items &&
           lhs.continuation_input_items == rhs.continuation_input_items && lhs.wire_snapshot == rhs.wire_snapshot &&
           same_output(lhs.output, rhs.output) && same_usage(lhs.usage, rhs.usage) &&
           same_error(lhs.error, rhs.error) && lhs.incomplete_details == rhs.incomplete_details;
}

common_json parse_arguments(const std::string & arguments) {
    if (arguments.empty()) {
        return common_json::object();
    }
    try {
        return common_json::parse(arguments);
    } catch (const std::exception &) {
        return nullptr;
    }
}

bool custom_input_if_present(const common_json & arguments, std::string & input) {
    if (!arguments.is_object()) {
        return false;
    }
    for (const char * key : { "input", "patch" }) {
        if (!arguments.contains(key)) {
            continue;
        }
        input = arguments.at(key).is_string() ? arguments.at(key).get<std::string>() : arguments.at(key).dump();
        return true;
    }
    return false;
}

std::string normalize_apply_patch_hunk_header(const std::string & line) {
    if (line.rfind("@@ -", 0) != 0) {
        return line;
    }
    const std::size_t new_range = line.find(" +", 4);
    if (new_range == std::string::npos) {
        return line;
    }
    const std::size_t range_end = line.find(" @@", new_range + 2);
    if (range_end == std::string::npos) {
        return line;
    }
    std::string context = line.substr(range_end + 3);
    while (!context.empty() && context.front() == ' ') {
        context.erase(context.begin());
    }
    return context.empty() ? "@@" : "@@ " + context;
}

// Keep this compatibility normalization aligned with
// tools/server/server-task.cpp::normalize_apply_patch_input until custom-tool
// lowering moves entirely into the typed sidecar domain.
std::string normalize_apply_patch_input(const std::string & input) {
    std::string output;
    output.reserve(input.size());
    std::size_t position = 0;
    while (position < input.size()) {
        const std::size_t line_end = input.find('\n', position);
        const std::size_t end      = line_end == std::string::npos ? input.size() : line_end;
        std::string       line     = input.substr(position, end - position);
        const bool        has_cr   = !line.empty() && line.back() == '\r';
        if (has_cr) {
            line.pop_back();
        }
        output += normalize_apply_patch_hunk_header(line);
        if (has_cr) {
            output += '\r';
        }
        if (line_end != std::string::npos) {
            output += '\n';
        }
        position = line_end == std::string::npos ? input.size() : line_end + 1;
    }
    return output;
}

std::optional<std::string> semantic_custom_input(const std::string & raw_arguments,
                                                 const std::string & name,
                                                 bool                terminal) {
    const common_json arguments = parse_arguments(raw_arguments);
    std::string       input;
    if (custom_input_if_present(arguments, input)) {
        // Normalizing a partially parsed patch could require retracting bytes.
        // Buffer it until the authoritative final parser snapshot instead.
        if (name == "apply_patch") {
            return terminal ? std::optional<std::string>(normalize_apply_patch_input(input)) : std::nullopt;
        }
        return input;
    }
    if (terminal) {
        return raw_arguments;
    }
    return std::nullopt;
}

generation_tool_kind tool_kind(const common_json & metadata) {
    const std::string type = metadata.is_object() ? metadata.value("type", std::string("function")) : "function";
    if (type == "function") {
        return generation_tool_kind::function;
    }
    if (type == "custom") {
        return generation_tool_kind::custom;
    }
    if (type == "local_shell") {
        return generation_tool_kind::local_shell;
    }
    throw std::invalid_argument("native Responses projection does not support generated tool type '" + type + "'");
}

std::string sse_frames(const std::vector<response_event> & events) {
    std::string output;
    for (const response_event & event : events) {
        const common_json rendered = render_generation_event(event);
        output += "event: ";
        output += rendered.at("type").get<std::string>();
        output += "\ndata: ";
        output += rendered.dump();
        output += "\n\n";
    }
    return output;
}

std::string storage_error_frame(std::uint64_t sequence_number, const std::string & message) {
    const common_json event = {
        { "type",            "error"                },
        { "sequence_number", sequence_number        },
        { "code",            "response_store_error" },
        { "message",         message                },
        { "param",           nullptr                },
    };
    return "event: error\ndata: " + event.dump() + "\n\n";
}

class server_delta_translator {
  public:
    explicit server_delta_translator(std::unordered_map<std::string, common_json> metadata) :
        metadata(std::move(metadata)) {}

    std::vector<generation_update> consume(const server_generation_update & update) {
        if (std::holds_alternative<server_generation_started>(update)) {
            return { generation_started{} };
        }
        if (std::holds_alternative<server_generation_progress>(update)) {
            return { generation_progress{} };
        }
        if (const auto * deltas = std::get_if<server_generation_message_deltas>(&update)) {
            return consume_deltas(*deltas);
        }
        if (const auto * snapshot = std::get_if<server_generation_message_snapshot>(&update)) {
            return consume_snapshot(*snapshot);
        }
        if (const auto * usage = std::get_if<server_generation_usage>(&update)) {
            return { generation_usage_update{ response_usage_from_server(*usage) } };
        }
        if (const auto * completed = std::get_if<server_generation_completed>(&update)) {
            return {
                generation_completed{
                                     response_usage_from_server(completed->usage),
                                     completed->completed_at,
                                     }
            };
        }
        if (const auto * incomplete = std::get_if<server_generation_incomplete>(&update)) {
            return {
                generation_incomplete{
                                      response_usage_from_server(incomplete->usage),
                                      incomplete->reason,
                                      }
            };
        }
        if (const auto * failed = std::get_if<server_generation_failed>(&update)) {
            std::optional<response_usage> usage;
            if (failed->usage) {
                usage = response_usage_from_server(*failed->usage);
            }
            return {
                generation_failed{
                                  response_error_from_server(failed->error),
                                  usage, }
            };
        }
        const auto &                  cancelled = std::get<server_generation_cancelled>(update);
        std::optional<response_usage> usage;
        if (cancelled.usage) {
            usage = response_usage_from_server(*cancelled.usage);
        }
        return { generation_cancelled{ usage } };
    }

  private:
    struct tool_decode_state {
        generation_tool_kind kind = generation_tool_kind::function;
        std::string          flattened_name;
        std::string          public_name;
        std::string          namespace_name;
        std::string          upstream_call_id;
        std::string          raw_arguments;
        std::string          emitted_custom_input;
        bool                 started = false;
    };

    std::map<std::size_t, tool_decode_state>     tools;
    std::unordered_map<std::string, common_json> metadata;
    std::optional<std::size_t>                   last_tool_index;

    std::size_t resolve_index(const common_chat_msg_diff & diff) {
        if (diff.tool_call_index != std::string::npos) {
            last_tool_index = diff.tool_call_index;
            return diff.tool_call_index;
        }
        if (diff.tool_call_delta.name.empty() && last_tool_index) {
            return *last_tool_index;
        }
        const std::size_t index = tools.empty() ? 0 : tools.rbegin()->first + 1;
        last_tool_index         = index;
        return index;
    }

    common_json metadata_for(const std::string & flattened_name) const {
        const auto found = metadata.find(flattened_name);
        return found == metadata.end() ? common_json::object() : found->second;
    }

    void start_tool(std::size_t index, tool_decode_state & tool, std::vector<generation_update> & output) {
        if (tool.started || tool.flattened_name.empty()) {
            return;
        }
        const common_json tool_metadata = metadata_for(tool.flattened_name);
        tool.kind                       = tool_kind(tool_metadata);
        tool.public_name =
            tool_metadata.is_object() ? tool_metadata.value("name", tool.flattened_name) : tool.flattened_name;
        tool.namespace_name =
            tool_metadata.is_object() ? tool_metadata.value("namespace", std::string()) : std::string();
        output.emplace_back(generation_tool_call_started{
            index,
            tool.kind,
            tool.public_name,
            tool.upstream_call_id,
            tool.namespace_name,
        });
        tool.started = true;

        if (tool.raw_arguments.empty()) {
            return;
        }
        if (tool.kind == generation_tool_kind::custom) {
            emit_custom_delta(index, tool, false, output);
        } else {
            output.emplace_back(generation_tool_call_delta{ index, tool.raw_arguments });
        }
    }

    static void emit_custom_delta(std::size_t                      index,
                                  tool_decode_state &              tool,
                                  bool                             terminal,
                                  std::vector<generation_update> & output) {
        const std::optional<std::string> current =
            semantic_custom_input(tool.raw_arguments, tool.public_name, terminal);
        if (!current) {
            return;
        }
        if (current->size() < tool.emitted_custom_input.size() ||
            current->compare(0, tool.emitted_custom_input.size(), tool.emitted_custom_input) != 0) {
            if (!terminal) {
                return;
            }
            // The terminal reconciliation below is authoritative. Streaming
            // bytes already emitted cannot be retracted, so do not emit a
            // misleading append delta for a non-monotonic correction.
            return;
        }
        const std::string delta   = current->substr(tool.emitted_custom_input.size());
        tool.emitted_custom_input = *current;
        if (!delta.empty()) {
            output.emplace_back(generation_tool_call_delta{ index, delta });
        }
    }

    void flush_pending_tools(std::optional<std::size_t> except_index, std::vector<generation_update> & output) {
        for (auto & entry : tools) {
            if ((!except_index || entry.first != *except_index) && !entry.second.started &&
                !entry.second.flattened_name.empty()) {
                start_tool(entry.first, entry.second, output);
            }
        }
    }

    std::vector<generation_update> consume_deltas(const server_generation_message_deltas & message) {
        std::vector<generation_update> output;
        for (const common_chat_msg_diff & diff : message.deltas) {
            const bool has_tool = diff.tool_call_index != std::string::npos || !diff.tool_call_delta.name.empty() ||
                                  !diff.tool_call_delta.id.empty() || !diff.tool_call_delta.arguments.empty();
            std::optional<std::size_t> index;
            if (has_tool) {
                index = resolve_index(diff);
            }
            flush_pending_tools(index, output);
            if (!diff.reasoning_content_delta.empty()) {
                output.emplace_back(generation_reasoning_delta{ diff.reasoning_content_delta });
            }
            if (!diff.content_delta.empty()) {
                output.emplace_back(generation_text_delta{ diff.content_delta });
            }
            if (!has_tool) {
                continue;
            }

            tool_decode_state & tool = tools[*index];
            if (!diff.tool_call_delta.id.empty()) {
                tool.upstream_call_id = diff.tool_call_delta.id;
            }
            if (!diff.tool_call_delta.name.empty()) {
                if (tool.started && tool.flattened_name != diff.tool_call_delta.name) {
                    throw std::logic_error("generated tool name changed after its native item was started");
                }
                tool.flattened_name = diff.tool_call_delta.name;
            }
            if (diff.tool_call_delta.arguments.empty()) {
                continue;
            }
            start_tool(*index, tool, output);
            tool.raw_arguments += diff.tool_call_delta.arguments;
            if (!tool.started) {
                continue;
            }
            if (tool.kind == generation_tool_kind::custom) {
                emit_custom_delta(*index, tool, false, output);
            } else {
                output.emplace_back(generation_tool_call_delta{ *index, diff.tool_call_delta.arguments });
            }
        }
        return output;
    }

    std::vector<generation_update> consume_snapshot(const server_generation_message_snapshot & snapshot) {
        std::vector<generation_update> output;

        // Reconcile text before allocating any tool that appeared only in the
        // final parser pass, preserving reasoning/message/tool output order.
        output.emplace_back(generation_message_reconciliation{
            snapshot.message.reasoning_content,
            snapshot.message.content,
            {},
        });

        std::vector<generation_tool_call_reconciliation> final_tools;
        final_tools.reserve(snapshot.message.tool_calls.size());
        for (std::size_t index = 0; index < snapshot.message.tool_calls.size(); ++index) {
            const common_chat_tool_call & final_call = snapshot.message.tool_calls[index];
            tool_decode_state &           tool       = tools[index];
            if (tool.started && tool.flattened_name != final_call.name) {
                throw std::logic_error("final generated tool name disagrees with its streamed name");
            }
            tool.flattened_name   = final_call.name;
            tool.upstream_call_id = final_call.id;
            start_tool(index, tool, output);
            if (!tool.started) {
                throw std::invalid_argument("final generated tool call has no name");
            }
            tool.raw_arguments      = final_call.arguments;
            std::string final_value = final_call.arguments;
            if (tool.kind == generation_tool_kind::custom) {
                final_value = semantic_custom_input(tool.raw_arguments, tool.public_name, true).value_or(final_value);
                emit_custom_delta(index, tool, true, output);
            }
            final_tools.push_back({ index, std::move(final_value) });
        }
        for (const auto & entry : tools) {
            if (entry.second.started && entry.first >= snapshot.message.tool_calls.size()) {
                throw std::logic_error("streamed generated tool call is absent from the final parser snapshot");
            }
        }
        output.emplace_back(generation_message_reconciliation{
            snapshot.message.reasoning_content,
            snapshot.message.content,
            std::move(final_tools),
        });
        return output;
    }
};

}  // namespace

class native_server_generation_sink::impl {
  public:
    impl(generation_response_context                  context,
         std::string                                  id_namespace,
         bool                                         stream,
         response_store *                             store,
         std::unordered_map<std::string, common_json> tool_metadata) :
        ids(std::move(id_namespace)),
        machine(std::move(context), ids),
        translator(std::move(tool_metadata)),
        stream(stream),
        store(store) {}

    std::string accept(const server_generation_update & update) {
        std::lock_guard<std::mutex> lock(mutex);
        if (checkpoint_failed) {
            return {};
        }

        const std::vector<generation_update> translated = translator.consume(update);
        std::vector<response_event>          events;
        for (const generation_update & native_update : translated) {
            std::vector<response_event> next = machine.apply(native_update);
            events.insert(events.end(), std::make_move_iterator(next.begin()), std::make_move_iterator(next.end()));
        }

        if (!checkpoint()) {
            cancellation_requested.store(true);
            discard_persisted_state_unlocked();
            checkpoint_failed = true;
            terminal_condition.notify_all();
            if (!stream) {
                throw std::runtime_error(checkpoint_error);
            }
            return storage_error_frame(next_stream_sequence++, checkpoint_error);
        }

        if (machine.terminal()) {
            terminal_condition.notify_all();
        }

        if (!stream) {
            return {};
        }
        const std::string output = sse_frames(events);
        if (!events.empty()) {
            next_stream_sequence = events.back().sequence_number + 1;
        }
        return output;
    }

    common_json snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (checkpoint_failed) {
            throw std::runtime_error(checkpoint_error);
        }
        return machine.snapshot();
    }

    response_id id() const {
        std::lock_guard<std::mutex> lock(mutex);
        return machine.state().id;
    }

    response_state state() const {
        std::lock_guard<std::mutex> lock(mutex);
        response_state              result = machine.state();
        result.revision                    = persisted_revision;
        return result;
    }

    bool terminal() const {
        std::lock_guard<std::mutex> lock(mutex);
        return machine.terminal();
    }

    bool wait_for_terminal(std::uint64_t timeout_ms) const {
        std::unique_lock<std::mutex> lock(mutex);
        return terminal_condition.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                           [this] { return machine.terminal() || checkpoint_failed; });
    }

    bool storage_failed() const {
        std::lock_guard<std::mutex> lock(mutex);
        return checkpoint_failed;
    }

    std::string storage_error() const {
        std::lock_guard<std::mutex> lock(mutex);
        return checkpoint_error;
    }

    void discard_persisted_state() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        discard_persisted_state_unlocked();
    }

    bool checkpoint() {
        if (store == nullptr) {
            return true;
        }
        try {
            response_state     current = machine.state();
            store_write_result result;
            std::uint64_t      next_revision;
            if (persisted_revision == 0) {
                result        = store->create(current);
                next_revision = 1;
            } else {
                if (persisted_state && persisted_state->detached_context) {
                    current.detached_context = persisted_state->detached_context;
                }
                current.revision = persisted_revision;
                result           = store->replace(current);
                next_revision    = persisted_revision + 1;

                if (result == store_write_result::stale_revision && persisted_state) {
                    const auto latest = store->find(current.id);
                    if (latest && same_generation_state(*latest, *persisted_state)) {
                        current.revision         = latest->revision;
                        current.detached_context = latest->detached_context;
                        result                   = store->replace(current);
                        next_revision            = latest->revision + 1;
                    }
                }
            }
            if (result != store_write_result::stored) {
                checkpoint_error = "The generated response could not be persisted (" +
                                   std::string(store_write_result_name(result)) + ")";
                return false;
            }
            persisted_revision = next_revision;
            current.revision   = persisted_revision;
            persisted_state    = std::move(current);
            return true;
        } catch (const std::exception & error) {
            checkpoint_error = std::string("The generated response could not be persisted: ") + error.what();
            return false;
        }
    }

    void discard_persisted_state_unlocked() noexcept {
        if (store == nullptr || persisted_revision == 0) {
            return;
        }
        try {
            store->erase(machine.state().id);
            // Cleanup is best effort; the owning route reports the original
            // generation or storage failure and must not replace it here.
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (const std::exception &) {
        }
        persisted_revision = 0;
    }

    counter_generation_id_source    ids;
    native_response_state_machine   machine;
    server_delta_translator         translator;
    const bool                      stream;
    response_store * const          store;
    mutable std::mutex              mutex;
    mutable std::condition_variable terminal_condition;
    std::atomic_bool                cancellation_requested{ false };
    std::uint64_t                   persisted_revision = 0;
    std::optional<response_state>   persisted_state;
    std::uint64_t                   next_stream_sequence = 0;
    bool                            checkpoint_failed    = false;
    std::string                     checkpoint_error;
};

native_server_generation_sink::native_server_generation_sink(
    generation_response_context                  context,
    std::string                                  id_namespace,
    bool                                         stream,
    response_store *                             store,
    std::unordered_map<std::string, common_json> tool_metadata) :
    implementation(
        std::make_unique<impl>(std::move(context), std::move(id_namespace), stream, store, std::move(tool_metadata))) {}

native_server_generation_sink::~native_server_generation_sink() = default;

std::string native_server_generation_sink::accept(const server_generation_update & update) {
    return implementation->accept(update);
}

common_json native_server_generation_sink::snapshot() const {
    return implementation->snapshot();
}

bool native_server_generation_sink::cancel_requested() const noexcept {
    return implementation->cancellation_requested.load();
}

void native_server_generation_sink::request_cancel() noexcept {
    implementation->cancellation_requested.store(true);
}

bool native_server_generation_sink::wait_for_terminal(std::uint64_t timeout_ms) const {
    return implementation->wait_for_terminal(timeout_ms);
}

response_id native_server_generation_sink::id() const {
    return implementation->id();
}

response_state native_server_generation_sink::state() const {
    return implementation->state();
}

bool native_server_generation_sink::terminal() const {
    return implementation->terminal();
}

bool native_server_generation_sink::storage_failed() const {
    return implementation->storage_failed();
}

std::string native_server_generation_sink::storage_error() const {
    return implementation->storage_error();
}

void native_server_generation_sink::discard_persisted_state() noexcept {
    implementation->discard_persisted_state();
}

}  // namespace llama_responses
