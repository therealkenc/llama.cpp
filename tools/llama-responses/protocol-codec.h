#ifndef LLAMA_RESPONSES_PROTOCOL_CODEC_H
#define LLAMA_RESPONSES_PROTOCOL_CODEC_H

#include "response-types.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace llama_responses {

using input_item_id_factory = std::function<item_id(std::size_t index, const common_json & item)>;

enum class input_item_order {
    ascending,
    descending,
};

struct input_item_page_options {
    std::size_t            limit = 20;
    input_item_order       order = input_item_order::descending;
    std::optional<item_id> after;
};

// Capture the create request's input as canonical Response input items. An id
// factory is optional because some callers have already assigned ids while
// others want their process-wide id source to do so here.
common_json capture_input_items(const common_json & normalized_request, const input_item_id_factory & make_id = {});

// Capture a generated wire item/response without discarding fields introduced
// by newer clients. Required identity fields are promoted into typed state.
response_output_item capture_output_item(const common_json & wire_item);
response_state       capture_response_state(const common_json & wire_response,
                                            const common_json & normalized_request = common_json::object(),
                                            const common_json & input_items        = nullptr);

common_json render_output_item(const response_output_item & item);
common_json render_response(const response_state & state);

// A route wrapper can use render_response() for the synchronous body, or put
// this JSON object through its SSE framing layer for a completed terminal
// event. The codec deliberately does not own HTTP headers or `data:` framing.
common_json render_terminal_event(const response_state & state, std::uint64_t sequence_number);

common_json render_deleted_response(const response_id & id);
common_json render_input_items_page(const common_json & input_items, const input_item_page_options & options = {});

// OpenAI-compatible API error envelope. param/code become JSON null when not
// supplied; the Response object's generation error is rendered separately by
// render_response().
common_json render_error(const std::string &                message,
                         const std::string &                type,
                         const std::optional<std::string> & param = std::nullopt,
                         const std::optional<std::string> & code  = std::nullopt);
common_json render_response_not_found(const response_id & id);

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_PROTOCOL_CODEC_H
