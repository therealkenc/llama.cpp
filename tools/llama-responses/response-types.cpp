#include "response-types.h"

namespace llama_responses {

const char * response_status_name(response_status status) noexcept {
    switch (status) {
        case response_status::queued:
            return "queued";
        case response_status::in_progress:
            return "in_progress";
        case response_status::completed:
            return "completed";
        case response_status::incomplete:
            return "incomplete";
        case response_status::failed:
            return "failed";
        case response_status::cancelled:
            return "cancelled";
    }
    return "failed";
}

bool response_status_is_terminal(response_status status) noexcept {
    return status == response_status::completed || status == response_status::incomplete ||
           status == response_status::failed || status == response_status::cancelled;
}

bool response_status_can_transition(response_status from, response_status to) noexcept {
    if (from == to) {
        return !response_status_is_terminal(from);
    }

    switch (from) {
        case response_status::queued:
            return to == response_status::in_progress || to == response_status::failed ||
                   to == response_status::cancelled;
        case response_status::in_progress:
            return response_status_is_terminal(to);
        case response_status::completed:
        case response_status::incomplete:
        case response_status::failed:
        case response_status::cancelled:
            return false;
    }
    return false;
}

const char * response_event_type_name(response_event_type type) noexcept {
    switch (type) {
        case response_event_type::created:
            return "response.created";
        case response_event_type::queued:
            return "response.queued";
        case response_event_type::in_progress:
            return "response.in_progress";
        case response_event_type::completed:
            return "response.completed";
        case response_event_type::incomplete:
            return "response.incomplete";
        case response_event_type::failed:
            return "response.failed";
        case response_event_type::cancelled:
            return "response.cancelled";
        case response_event_type::output_item_added:
            return "response.output_item.added";
        case response_event_type::output_item_done:
            return "response.output_item.done";
        case response_event_type::content_part_added:
            return "response.content_part.added";
        case response_event_type::content_part_done:
            return "response.content_part.done";
        case response_event_type::output_text_delta:
            return "response.output_text.delta";
        case response_event_type::output_text_done:
            return "response.output_text.done";
        case response_event_type::function_call_arguments_delta:
            return "response.function_call_arguments.delta";
        case response_event_type::function_call_arguments_done:
            return "response.function_call_arguments.done";
        case response_event_type::custom_tool_call_input_delta:
            return "response.custom_tool_call_input.delta";
        case response_event_type::custom_tool_call_input_done:
            return "response.custom_tool_call_input.done";
        case response_event_type::reasoning_summary_part_added:
            return "response.reasoning_summary_part.added";
        case response_event_type::reasoning_summary_part_done:
            return "response.reasoning_summary_part.done";
        case response_event_type::reasoning_summary_text_delta:
            return "response.reasoning_summary_text.delta";
        case response_event_type::reasoning_summary_text_done:
            return "response.reasoning_summary_text.done";
        case response_event_type::reasoning_text_delta:
            return "response.reasoning_text.delta";
        case response_event_type::reasoning_text_done:
            return "response.reasoning_text.done";
        case response_event_type::error:
            return "error";
    }
    return "error";
}

}  // namespace llama_responses
