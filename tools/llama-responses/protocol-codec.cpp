#include "protocol-codec.h"

#include "json.h"
#include "response-types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace llama_responses {
namespace {

std::string string_field(const common_json & object, const char * key, const std::string & fallback = {}) {
    return object.is_object() && object.contains(key) && object.at(key).is_string() ?
               object.at(key).get<std::string>() :
               fallback;
}

std::uint64_t uint_field(const common_json & object, const char * key, std::uint64_t fallback = 0) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_number_integer()) {
        return fallback;
    }
    const long long value = object.at(key).get<long long>();
    return value < 0 ? fallback : static_cast<std::uint64_t>(value);
}

response_status parse_status(const std::string & value) {
    if (value == "queued") {
        return response_status::queued;
    }
    if (value == "in_progress") {
        return response_status::in_progress;
    }
    if (value == "completed") {
        return response_status::completed;
    }
    if (value == "incomplete") {
        return response_status::incomplete;
    }
    if (value == "failed") {
        return response_status::failed;
    }
    if (value == "cancelled") {
        return response_status::cancelled;
    }
    throw std::invalid_argument("unsupported response status: " + value);
}

common_json canonical_input_item(common_json item) {
    if (item.is_string()) {
        item = {
            { "type",    "message"                    },
            { "role",    "user"                       },
            { "content", common_json::array({ common_json{
                             { "type", "input_text" },
                             { "text", item.get<std::string>() },
                         } }) },
        };
    }

    if (!item.is_object()) {
        throw std::invalid_argument("Responses input items must be strings or objects");
    }

    // EasyInputMessage permits a string content value. Stored input items use
    // the canonical item-list shape returned by the OpenAI input-items route.
    if (item.contains("content") && item.at("content").is_string()) {
        item["content"] = common_json::array({
            common_json{
                        { "type", "input_text" },
                        { "text", item.at("content").get<std::string>() },
                        }
        });
    }
    if (item.contains("role") && !item.contains("type")) {
        item["type"] = "message";
    }
    return item;
}

void set_request_field(common_json & response, const common_json & request, const char * key, common_json fallback) {
    if (response.contains(key)) {
        return;
    }
    if (request.is_object() && request.contains(key)) {
        response[key] = request.at(key);
    } else {
        response[key] = std::move(fallback);
    }
}

common_json render_usage(const response_usage & usage, common_json result = common_json::object()) {
    if (!result.is_object()) {
        result = common_json::object();
    }
    common_json input_details =
        result.contains("input_tokens_details") && result.at("input_tokens_details").is_object() ?
            result.at("input_tokens_details") :
            common_json::object();
    common_json output_details =
        result.contains("output_tokens_details") && result.at("output_tokens_details").is_object() ?
            result.at("output_tokens_details") :
            common_json::object();
    input_details["cached_tokens"]     = usage.cached_input_tokens;
    output_details["reasoning_tokens"] = usage.reasoning_output_tokens;
    result["input_tokens"]             = usage.input_tokens;
    result["input_tokens_details"]     = std::move(input_details);
    result["output_tokens"]            = usage.output_tokens;
    result["output_tokens_details"]    = std::move(output_details);
    result["total_tokens"]             = usage.total_tokens();
    return result;
}

std::string output_text(const common_json & output) {
    std::string text;
    if (!output.is_array()) {
        return text;
    }
    for (const common_json & item : output) {
        if (!item.is_object() || string_field(item, "type") != "message" || !item.contains("content") ||
            !item.at("content").is_array()) {
            continue;
        }
        for (const common_json & part : item.at("content")) {
            if (part.is_object() && string_field(part, "type") == "output_text" && part.contains("text") &&
                part.at("text").is_string()) {
                text += part.at("text").get<std::string>();
            }
        }
    }
    return text;
}

std::string input_item_id(const common_json & item) {
    return string_field(item, "id");
}

}  // namespace

common_json capture_input_items(const common_json & normalized_request, const input_item_id_factory & make_id) {
    common_json input = normalized_request;
    if (normalized_request.is_object()) {
        if (!normalized_request.contains("input")) {
            return common_json::array();
        }
        input = normalized_request.at("input");
    }

    if (input.is_null()) {
        return common_json::array();
    }
    if (input.is_string() || input.is_object()) {
        input = common_json::array({ input });
    }
    if (!input.is_array()) {
        throw std::invalid_argument("'input' must be a string, object, or array of items");
    }

    common_json result = common_json::array();
    std::size_t index  = 0;
    for (const common_json & value : input) {
        common_json item = canonical_input_item(value);
        if (item.contains("id") && (!item.at("id").is_string() || item.at("id").get<std::string>().empty())) {
            throw std::invalid_argument("Responses input item id must be a non-empty string");
        }
        if (!item.contains("id") && make_id) {
            const item_id id = make_id(index, item);
            if (!id.empty()) {
                item["id"] = id.str();
            }
        }
        result.push_back(std::move(item));
        ++index;
    }
    return result;
}

response_output_item capture_output_item(const common_json & wire_item) {
    if (!wire_item.is_object()) {
        throw std::invalid_argument("response output item must be an object");
    }

    const std::string id   = string_field(wire_item, "id");
    const std::string type = string_field(wire_item, "type");
    if (id.empty() || type.empty()) {
        throw std::invalid_argument("response output item requires string id and type fields");
    }

    response_output_item result;
    result.id              = item_id(id);
    result.type            = type;
    result.value           = wire_item;
    const std::string call = string_field(wire_item, "call_id");
    if (!call.empty()) {
        result.call = call_id(call);
    }
    return result;
}

response_state capture_response_state(const common_json & wire_response,
                                      const common_json & normalized_request,
                                      const common_json & input_items) {
    if (!wire_response.is_object()) {
        throw std::invalid_argument("response snapshot must be an object");
    }

    const std::string id = string_field(wire_response, "id");
    if (id.empty()) {
        throw std::invalid_argument("response snapshot requires a string id");
    }

    response_state state;
    state.id         = response_id(id);
    state.status     = parse_status(string_field(wire_response, "status", "completed"));
    state.created_at = uint_field(wire_response, "created_at");
    state.model      = string_field(wire_response, "model", string_field(normalized_request, "model"));
    state.request    = normalized_request;
    state.input_items =
        input_items.is_null() ?
            capture_input_items(normalized_request,
                                [&state](std::size_t index, const common_json & item) {
                                    const std::string prefix =
                                        item.value("type", std::string()) == "message" ? "msg_" : "item_";
                                    return item_id(prefix + state.id.str() + "_input_" + std::to_string(index));
                                }) :
            input_items;
    state.wire_snapshot = wire_response;

    if (wire_response.contains("completed_at") && wire_response.at("completed_at").is_number_integer()) {
        state.completed_at = uint_field(wire_response, "completed_at");
    }
    const std::string previous =
        string_field(wire_response, "previous_response_id", string_field(normalized_request, "previous_response_id"));
    if (!previous.empty()) {
        state.previous_response = response_id(previous);
    }
    if (wire_response.contains("metadata") && wire_response.at("metadata").is_object()) {
        state.metadata = wire_response.at("metadata");
    } else if (normalized_request.is_object() && normalized_request.contains("metadata") &&
               normalized_request.at("metadata").is_object()) {
        state.metadata = normalized_request.at("metadata");
    }
    if (wire_response.contains("incomplete_details")) {
        state.incomplete_details = wire_response.at("incomplete_details");
    }
    if (wire_response.contains("error") && wire_response.at("error").is_object()) {
        response_error error;
        error.code    = string_field(wire_response.at("error"), "code");
        error.message = string_field(wire_response.at("error"), "message");
        error.param   = string_field(wire_response.at("error"), "param");
        state.error   = std::move(error);
    }
    if (wire_response.contains("usage") && wire_response.at("usage").is_object()) {
        const common_json & usage = wire_response.at("usage");
        state.usage.input_tokens  = uint_field(usage, "input_tokens");
        state.usage.output_tokens = uint_field(usage, "output_tokens");
        if (usage.contains("input_tokens_details") && usage.at("input_tokens_details").is_object()) {
            state.usage.cached_input_tokens = uint_field(usage.at("input_tokens_details"), "cached_tokens");
        }
        if (usage.contains("output_tokens_details") && usage.at("output_tokens_details").is_object()) {
            state.usage.reasoning_output_tokens = uint_field(usage.at("output_tokens_details"), "reasoning_tokens");
        }
    }
    if (wire_response.contains("output") && wire_response.at("output").is_array()) {
        for (const common_json & item : wire_response.at("output")) {
            state.output.push_back(capture_output_item(item));
        }
    }
    return state;
}

common_json render_output_item(const response_output_item & item) {
    common_json result = item.value.is_object() ? item.value : common_json::object();
    result["id"]       = item.id.str();
    result["type"]     = item.type;
    if (item.call) {
        result["call_id"] = item.call->str();
    } else {
        result.erase("call_id");
    }
    return result;
}

common_json render_response(const response_state & state) {
    common_json result = state.wire_snapshot.is_object() ? state.wire_snapshot : common_json::object();
    common_json output = common_json::array();
    for (const response_output_item & item : state.output) {
        output.push_back(render_output_item(item));
    }

    result["id"]           = state.id.str();
    result["object"]       = "response";
    result["created_at"]   = state.created_at;
    result["status"]       = response_status_name(state.status);
    result["model"]        = state.model;
    result["output"]       = output;
    result["output_text"]  = output_text(output);
    result["completed_at"] = state.completed_at ? common_json(*state.completed_at) : common_json(nullptr);
    result["previous_response_id"] =
        state.previous_response ? common_json(state.previous_response->str()) : common_json(nullptr);
    result["incomplete_details"] = state.incomplete_details;
    if (state.error) {
        common_json error =
            result.contains("error") && result.at("error").is_object() ? result.at("error") : common_json::object();
        error["code"]    = state.error->code;
        error["message"] = state.error->message;
        if (!state.error->param.empty()) {
            error["param"] = state.error->param;
        }
        result["error"] = std::move(error);
    } else {
        result["error"] = nullptr;
    }
    result["metadata"] = state.metadata;
    if (!response_status_is_terminal(state.status)) {
        result["usage"] = nullptr;
    } else if (!result.contains("usage") || !result.at("usage").is_null()) {
        result["usage"] = render_usage(state.usage, result.value("usage", common_json::object()));
    }

    // Defaults and field selection are intentionally shared in spirit with
    // tools/server/server-task.cpp::build_oai_resp_metadata. Keeping this
    // codec independent lets the old implementation remain an oracle while
    // newer wire fields survive via wire_snapshot.
    set_request_field(result, state.request, "instructions", nullptr);
    set_request_field(result, state.request, "tools", common_json::array());
    set_request_field(result, state.request, "tool_choice", "auto");
    set_request_field(result, state.request, "truncation", "disabled");
    set_request_field(result, state.request, "parallel_tool_calls", true);
    set_request_field(result, state.request, "text",
                      common_json{
                          { "format", common_json{ { "type", "text" } } }
    });
    set_request_field(result, state.request, "top_p", 1.0);
    set_request_field(result, state.request, "presence_penalty", 0.0);
    set_request_field(result, state.request, "frequency_penalty", 0.0);
    set_request_field(result, state.request, "top_logprobs", 0);
    set_request_field(result, state.request, "temperature", 1.0);
    set_request_field(result, state.request, "reasoning", nullptr);
    set_request_field(result, state.request, "max_output_tokens", nullptr);
    set_request_field(result, state.request, "max_tool_calls", nullptr);
    set_request_field(result, state.request, "store", true);
    set_request_field(result, state.request, "background", false);
    set_request_field(result, state.request, "service_tier", "default");
    set_request_field(result, state.request, "safety_identifier", nullptr);
    set_request_field(result, state.request, "prompt_cache_key", nullptr);
    set_request_field(result, state.request, "prompt_cache_options", nullptr);
    set_request_field(result, state.request, "prompt_cache_retention", nullptr);
    set_request_field(result, state.request, "prompt", nullptr);
    set_request_field(result, state.request, "conversation", nullptr);
    set_request_field(result, state.request, "user", nullptr);

    return result;
}

common_json render_terminal_event(const response_state & state, std::uint64_t sequence_number) {
    if (!response_status_is_terminal(state.status)) {
        throw std::invalid_argument("cannot render terminal event for a non-terminal response");
    }

    const char * type = "response.failed";
    switch (state.status) {
        case response_status::completed:
            type = "response.completed";
            break;
        case response_status::incomplete:
            type = "response.incomplete";
            break;
        case response_status::failed:
            type = "response.failed";
            break;
        case response_status::cancelled:
            type = "response.cancelled";
            break;
        case response_status::queued:
        case response_status::in_progress:
            break;
    }
    return {
        { "type",            type                   },
        { "sequence_number", sequence_number        },
        { "response",        render_response(state) },
    };
}

common_json render_deleted_response(const response_id & id) {
    return {
        { "id",      id.str()   },
        { "object",  "response" },
        { "deleted", true       },
    };
}

common_json render_input_items_page(const common_json & input_items, const input_item_page_options & options) {
    if (!input_items.is_array()) {
        throw std::invalid_argument("stored response input_items must be an array");
    }
    if (options.limit < 1 || options.limit > 100) {
        throw std::invalid_argument("input item page limit must be between 1 and 100");
    }

    std::vector<common_json> ordered;
    ordered.reserve(input_items.size());
    for (const common_json & item : input_items) {
        ordered.push_back(item);
    }
    if (options.order == input_item_order::descending) {
        std::reverse(ordered.begin(), ordered.end());
    }

    std::size_t start = 0;
    if (options.after) {
        const auto cursor = std::find_if(ordered.begin(), ordered.end(), [&](const common_json & item) {
            return input_item_id(item) == options.after->str();
        });
        if (cursor == ordered.end()) {
            throw std::invalid_argument("input item cursor was not found");
        }
        start = static_cast<std::size_t>(std::distance(ordered.begin(), cursor)) + 1;
    }

    const std::size_t count = std::min(options.limit, ordered.size() - start);
    common_json       data  = common_json::array();
    for (std::size_t index = 0; index < count; ++index) {
        data.push_back(ordered[start + index]);
    }

    common_json result = {
        { "object",   "list"                         },
        { "data",     data                           },
        { "first_id", nullptr                        },
        { "last_id",  nullptr                        },
        { "has_more", start + count < ordered.size() },
    };
    if (!data.empty()) {
        const std::string first = input_item_id(data.front());
        const std::string last  = input_item_id(data.back());
        if (!first.empty()) {
            result["first_id"] = first;
        }
        if (!last.empty()) {
            result["last_id"] = last;
        }
    }
    return result;
}

common_json render_error(const std::string &                message,
                         const std::string &                type,
                         const std::optional<std::string> & param,
                         const std::optional<std::string> & code) {
    return {
        { "error",
         common_json{
              { "message", message },
              { "type", type },
              { "param", param ? common_json(*param) : common_json(nullptr) },
              { "code", code ? common_json(*code) : common_json(nullptr) },
          } },
    };
}

common_json render_response_not_found(const response_id & id) {
    return render_error("No response found with id '" + id.str() + "'.", "invalid_request_error",
                        std::string("response_id"), std::string("response_not_found"));
}

}  // namespace llama_responses
