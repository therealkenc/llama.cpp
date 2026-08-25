#include "server-generation.h"

#include "chat.h"
#include "server-common.h"
#include "server-task.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <optional>
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
            updates.emplace_back(server_generation_message_deltas{
                partial->oaicompat_msg_diffs,
                partial->responses_tool_metadata,
            });
        }
        return updates;
    }

    if (const auto * final = dynamic_cast<const server_task_result_cmpl_final *>(&result)) {
        if (!final->oaicompat_msg_diffs.empty()) {
            updates.emplace_back(server_generation_message_deltas{
                final->oaicompat_msg_diffs,
                final->generation_params.responses_tool_metadata,
            });
        }
        common_chat_msg snapshot = final->oaicompat_msg;
        if (snapshot.empty()) {
            // Match server_task_result_cmpl_final::to_json_oaicompat_resp(): a
            // parser which cannot recognize the model output must not erase
            // the raw generated text from the native Responses projection.
            snapshot.role    = "assistant";
            snapshot.content = final->content;
        }
        updates.emplace_back(server_generation_message_snapshot{
            std::move(snapshot),
            final->generation_params.responses_tool_metadata,
        });
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
