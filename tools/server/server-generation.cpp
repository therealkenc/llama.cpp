#include "server-generation.h"

#include "chat.h"
#include "server-common.h"
#include "server-generation-internal.h"
#include "server-task.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

std::uint64_t token_count(std::int32_t value) {
    return static_cast<std::uint64_t>(std::max<std::int32_t>(value, 0));
}

server_generation_usage generation_usage(const server_task_result_cmpl_final & result) {
    return {
        token_count(result.n_prompt_tokens),
        token_count(result.n_prompt_tokens_cache),
        token_count(result.n_decoded),
    };
}

std::string generation_error_code(error_type type) {
    switch (type) {
        case ERROR_TYPE_INVALID_REQUEST:
            return "invalid_request";
        case ERROR_TYPE_AUTHENTICATION:
            return "authentication_error";
        case ERROR_TYPE_SERVER:
            return "server_error";
        case ERROR_TYPE_NOT_FOUND:
            return "not_found_error";
        case ERROR_TYPE_PERMISSION:
            return "permission_error";
        case ERROR_TYPE_UNAVAILABLE:
            return "server_overloaded";
        case ERROR_TYPE_NOT_SUPPORTED:
            return "not_supported_error";
        case ERROR_TYPE_EXCEED_CONTEXT_SIZE:
            return "context_length_exceeded";
    }
    return "server_error";
}

}  // namespace

bool server_generation_update_is_terminal(const server_generation_update & update) noexcept {
    return std::holds_alternative<server_generation_completed>(update) ||
           std::holds_alternative<server_generation_incomplete>(update) ||
           std::holds_alternative<server_generation_failed>(update) ||
           std::holds_alternative<server_generation_cancelled>(update);
}

std::vector<server_generation_update> server_generation_updates_from_result(const server_task_result & result,
                                                                            bool include_started) {
    std::vector<server_generation_update> updates;
    if (include_started) {
        updates.emplace_back(server_generation_started{});
    }

    if (const auto * partial = dynamic_cast<const server_task_result_cmpl_partial *>(&result)) {
        if (partial->is_begin) {
            return updates;
        }
        if (partial->is_progress) {
            updates.emplace_back(server_generation_progress{});
        }
        if (!partial->oaicompat_msg_diffs.empty()) {
            updates.emplace_back(server_generation_message_deltas{ partial->oaicompat_msg_diffs });
        }
        return updates;
    }

    if (const auto * final = dynamic_cast<const server_task_result_cmpl_final *>(&result)) {
        if (!final->oaicompat_msg_diffs.empty()) {
            updates.emplace_back(server_generation_message_deltas{ final->oaicompat_msg_diffs });
        }
        common_chat_msg snapshot = final->oaicompat_msg;
        if (snapshot.empty()) {
            // Match server_task_result_cmpl_final::to_json_oaicompat_resp(): a
            // parser which cannot recognize the model output must not erase
            // the raw generated text from the native Responses projection.
            snapshot.role    = "assistant";
            snapshot.content = final->content;
        }
        updates.emplace_back(server_generation_message_snapshot{ std::move(snapshot) });
        const server_generation_usage usage = generation_usage(*final);
        if (final->stop == STOP_TYPE_LIMIT) {
            updates.emplace_back(server_generation_incomplete{
                usage,
                "max_output_tokens",
            });
        } else {
            updates.emplace_back(server_generation_completed{
                usage,
                static_cast<std::uint64_t>(std::time(nullptr)),
            });
        }
        return updates;
    }

    if (const auto * error = dynamic_cast<const server_task_result_error *>(&result)) {
        updates.emplace_back(server_generation_failed{
            {
             generation_error_code(error->err_type),
             error->err_msg,
             "", },
            std::nullopt,
        });
        return updates;
    }

    throw std::invalid_argument("server generation sink received an unsupported task result");
}

json server_generation_params_parse(const server_generation_input & input,
                                    const server_chat_params &      options,
                                    std::vector<raw_buffer> &       files,
                                    bool                            load_media) {
    if (!input.inference_parameters.is_object()) {
        throw std::invalid_argument("generation inference_parameters must be an object");
    }

    static constexpr std::array<const char *, 17> reserved_parameters = {
        "__llama_responses_request",
        "__responses_tool_metadata",
        "chat_parser",
        "chat_template_kwargs",
        "generation_prompt",
        "grammar",
        "grammar_triggers",
        "json_schema",
        "message_delimiters",
        "messages",
        "parallel_tool_calls",
        "parse_tool_calls",
        "preserved_tokens",
        "prompt",
        "response_format",
        "tool_choice",
        "tools",
    };
    for (const char * key : reserved_parameters) {
        if (input.inference_parameters.contains(key)) {
            throw std::invalid_argument(std::string("generation inference_parameters contains reserved field: ") + key);
        }
    }

    json body     = input.inference_parameters;
    json messages = json::array();
    for (const common_chat_msg & message : input.chat.messages) {
        if (!message.content.empty() && !message.content_parts.empty()) {
            throw std::invalid_argument("generation message cannot contain both text and typed content parts");
        }
        json encoded = message.to_json_oaicompat(false);
        if (!message.content_parts.empty()) {
            encoded["content"] = json::array();
            for (const common_chat_msg_content_part & part : message.content_parts) {
                encoded["content"].push_back({
                    { "type", part.type },
                    { "text", part.text },
                });
            }
        }
        messages.push_back(std::move(encoded));
    }

    std::optional<std::pair<std::size_t, std::size_t>> previous_coordinate;
    std::set<std::pair<std::size_t, std::size_t>>      media_coordinates;
    for (const server_generation_media_source & media : input.media) {
        const std::pair<std::size_t, std::size_t> coordinate = {
            media.message_index,
            media.content_part_index,
        };
        if (previous_coordinate && coordinate <= *previous_coordinate) {
            throw std::invalid_argument("generation media coordinates must be unique and in prompt order");
        }
        previous_coordinate = coordinate;
        media_coordinates.insert(coordinate);
        if (media.source.empty()) {
            throw std::invalid_argument("generation media source is empty");
        }
        if (media.message_index >= messages.size() || !messages.at(media.message_index).contains("content") ||
            !messages.at(media.message_index).at("content").is_array() ||
            media.content_part_index >= messages.at(media.message_index).at("content").size()) {
            throw std::invalid_argument("generation media coordinate is outside the message content");
        }
        json & part = messages[media.message_index]["content"][media.content_part_index];
        if (!part.is_object() || json_value(part, "type", std::string()) != "media_marker") {
            throw std::invalid_argument("generation media coordinate does not identify a media marker");
        }

        if (!load_media) {
            part = {
                { "type", "text"             },
                { "text", get_media_marker() },
            };
            continue;
        }
        switch (media.kind) {
            case server_generation_media_kind::image:
                part = {
                    { "type",      "image_url"                 },
                    { "image_url", { { "url", media.source } } },
                };
                break;
            case server_generation_media_kind::audio:
                part = {
                    { "type",        "input_audio"                },
                    { "input_audio", { { "data", media.source } } },
                };
                break;
            case server_generation_media_kind::video:
                part = {
                    { "type",        "input_video"                },
                    { "input_video", { { "data", media.source } } },
                };
                break;
        }
    }
    for (std::size_t message_index = 0; message_index < messages.size(); ++message_index) {
        const json & message = messages.at(message_index);
        if (!message.contains("content") || !message.at("content").is_array()) {
            continue;
        }
        for (std::size_t part_index = 0; part_index < message.at("content").size(); ++part_index) {
            const json & part = message.at("content").at(part_index);
            if (part.is_object() && json_value(part, "type", std::string()) == "media_marker" &&
                media_coordinates.find({ message_index, part_index }) == media_coordinates.end()) {
                throw std::invalid_argument("generation media marker has no matching source");
            }
        }
    }

    body["messages"]              = std::move(messages);
    body["add_generation_prompt"] = input.chat.add_generation_prompt;
    if (!input.chat.tools.empty()) {
        body["tools"] = common_chat_tools_to_json_oaicompat(input.chat.tools);
    }
    switch (input.chat.tool_choice) {
        case COMMON_CHAT_TOOL_CHOICE_AUTO:
            body["tool_choice"] = "auto";
            break;
        case COMMON_CHAT_TOOL_CHOICE_REQUIRED:
            body["tool_choice"] = "required";
            break;
        case COMMON_CHAT_TOOL_CHOICE_NONE:
            body["tool_choice"] = "none";
            break;
    }
    if (input.parallel_tool_calls) {
        body["parallel_tool_calls"] = *input.parallel_tool_calls;
    }
    if (!input.chat.json_schema.empty()) {
        body["json_schema"] = json::parse(input.chat.json_schema);
    }
    if (!input.chat.grammar.empty()) {
        body["grammar"] = input.chat.grammar;
    }
    if (!input.chat.chat_template_kwargs.empty()) {
        json kwargs = json::object();
        for (const auto & item : input.chat.chat_template_kwargs) {
            kwargs[item.first] = json::parse(item.second);
        }
        body["chat_template_kwargs"] = std::move(kwargs);
    }
    switch (input.chat.continue_final_message) {
        case COMMON_CHAT_CONTINUATION_NONE:
            break;
        case COMMON_CHAT_CONTINUATION_AUTO:
            body["continue_final_message"] = true;
            break;
        case COMMON_CHAT_CONTINUATION_REASONING:
            body["continue_final_message"] = "reasoning_content";
            break;
        case COMMON_CHAT_CONTINUATION_CONTENT:
            body["continue_final_message"] = "content";
            break;
    }

    try {
        // Reuse llama-server's prompt/sampling parser in-process. No HTTP
        // request or Chat Completions response shape crosses this boundary.
        return oaicompat_chat_params_parse(body, options, files);
    } catch (const std::invalid_argument &) {
        throw;
    } catch (const std::exception & error) {
        // Everything above generation is request/template/media validation.
        // Surface model capability and media-loader failures as a bad request,
        // not as an internal server failure.
        throw std::invalid_argument(error.what());
    }
}

struct server_generation_projection::state {
    explicit state(server_generation_sink_ptr sink) : sink(std::move(sink)) {}

    ~state() {
        if (sink == nullptr || terminal) {
            return;
        }
        try {
            // A streaming HTTP response can be destroyed after a failed socket
            // write without another call to its `next` closure. Persist a
            // terminal cancellation before reader RAII abandons the task.
            std::vector<server_generation_update> updates;
            if (!started) {
                updates.emplace_back(server_generation_started{});
            }
            updates.emplace_back(server_generation_cancelled{});
            for (const server_generation_update & update : updates) {
                (void) sink->accept(update);
            }
        } catch (const std::exception & error) {
            SRV_ERR("failed to terminalize an abandoned generation projection: %s\n", error.what());
        }
    }

    server_generation_sink_ptr sink;
    std::string                pending_output;
    bool                       started  = false;
    bool                       terminal = false;
};

server_generation_projection::server_generation_projection(server_generation_sink_ptr sink) :
    state_(std::make_shared<state>(std::move(sink))) {}

bool server_generation_projection::enabled() const noexcept {
    return state_->sink != nullptr;
}

bool server_generation_projection::cancel_requested() const noexcept {
    return state_->sink != nullptr && !state_->terminal && state_->sink->cancel_requested();
}

std::string server_generation_projection::accept(const server_task_result & result) {
    return accept(server_generation_updates_from_result(result, !state_->started));
}

std::string server_generation_projection::cancel() {
    if (state_->sink == nullptr || state_->terminal) {
        return {};
    }
    std::vector<server_generation_update> updates;
    if (!state_->started) {
        updates.emplace_back(server_generation_started{});
    }
    updates.emplace_back(server_generation_cancelled{});
    return accept(updates);
}

std::string server_generation_projection::fail(const std::string & message) {
    if (state_->sink == nullptr || state_->terminal) {
        return {};
    }
    std::vector<server_generation_update> updates;
    if (!state_->started) {
        updates.emplace_back(server_generation_started{});
    }
    updates.emplace_back(server_generation_failed{
        { "server_error", message, "" },
        std::nullopt,
    });
    return accept(updates);
}

common_json server_generation_projection::snapshot() const {
    if (state_->sink == nullptr) {
        throw std::logic_error("generation projection has no sink");
    }
    return state_->sink->snapshot();
}

std::string server_generation_projection::accept(const std::vector<server_generation_update> & updates) {
    if (state_->sink == nullptr) {
        throw std::logic_error("generation projection has no sink");
    }
    for (const server_generation_update & update : updates) {
        state_->pending_output += state_->sink->accept(update);
        state_->started |= std::holds_alternative<server_generation_started>(update);
        state_->terminal |= server_generation_update_is_terminal(update);
    }
    return std::exchange(state_->pending_output, {});
}
