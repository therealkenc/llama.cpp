#ifndef LLAMA_RESPONSES_RESPONSE_TYPES_H
#define LLAMA_RESPONSES_RESPONSE_TYPES_H

#include "json.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llama_responses {

// Keep the identifiers used by the Responses wire format distinct in C++.
// In particular, a function-call output item id (normally `fc_...`) and the
// call id echoed by a function_call_output (normally `call_...`) are not
// interchangeable. tools/server/server-task.cpp historically blurred these
// values, which makes item replay unnecessarily fragile.
template <typename Tag> class opaque_id {
  public:
    opaque_id() = default;

    explicit opaque_id(std::string value) : value_(std::move(value)) {}

    const std::string & str() const noexcept { return value_; }

    bool empty() const noexcept { return value_.empty(); }

    explicit operator bool() const noexcept { return !empty(); }

    friend bool operator==(const opaque_id & lhs, const opaque_id & rhs) noexcept { return lhs.value_ == rhs.value_; }

    friend bool operator!=(const opaque_id & lhs, const opaque_id & rhs) noexcept { return !(lhs == rhs); }

    friend bool operator<(const opaque_id & lhs, const opaque_id & rhs) noexcept { return lhs.value_ < rhs.value_; }

  private:
    std::string value_;
};

struct response_id_tag;
struct item_id_tag;
struct call_id_tag;

using response_id = opaque_id<response_id_tag>;
using item_id     = opaque_id<item_id_tag>;
using call_id     = opaque_id<call_id_tag>;

enum class response_status {
    queued,
    in_progress,
    completed,
    incomplete,
    failed,
    cancelled,
};

const char * response_status_name(response_status status) noexcept;
bool         response_status_is_terminal(response_status status) noexcept;
bool         response_status_can_transition(response_status from, response_status to) noexcept;

// Event names intentionally mirror the public Responses API. The event body
// remains extensible JSON while identity, ordering, and item coordinates are
// typed. This keeps protocol growth from forcing a variant with dozens of
// nearly-identical payload structures.
enum class response_event_type {
    created,
    queued,
    in_progress,
    completed,
    incomplete,
    failed,
    cancelled,
    output_item_added,
    output_item_done,
    content_part_added,
    content_part_done,
    output_text_delta,
    output_text_done,
    function_call_arguments_delta,
    function_call_arguments_done,
    custom_tool_call_input_delta,
    custom_tool_call_input_done,
    reasoning_summary_part_added,
    reasoning_summary_part_done,
    reasoning_summary_text_delta,
    reasoning_summary_text_done,
    error,
};

const char * response_event_type_name(response_event_type type) noexcept;

struct response_event {
    response_event_type        type            = response_event_type::created;
    std::uint64_t              sequence_number = 0;
    response_id                response;
    std::optional<item_id>     item;
    std::optional<std::size_t> output_index;
    std::optional<std::size_t> content_index;
    common_json                data = common_json::object();
};

struct response_usage {
    std::uint64_t input_tokens            = 0;
    std::uint64_t cached_input_tokens     = 0;
    std::uint64_t output_tokens           = 0;
    std::uint64_t reasoning_output_tokens = 0;

    std::uint64_t total_tokens() const noexcept { return input_tokens + output_tokens; }
};

struct response_error {
    std::string code;
    std::string message;
    std::string param;
};

struct response_output_item {
    item_id                id;
    std::optional<call_id> call;
    std::string            type;
    common_json            value = common_json::object();
};

// Durable state owned by the Responses layer. `revision` is maintained by a
// response_store and supplies optimistic concurrency for streaming updates.
// request is the normalized request used to create the response; output items
// are stored independently indexable so item_reference can be resolved later.
struct response_state {
    response_id                       id;
    response_status                   status     = response_status::in_progress;
    std::uint64_t                     created_at = 0;
    std::optional<std::uint64_t>      completed_at;
    std::uint64_t                     revision             = 0;
    std::uint64_t                     next_sequence_number = 0;
    std::string                       model;
    std::optional<response_id>        previous_response;
    common_json                       request       = common_json::object();
    common_json                       metadata      = common_json::object();
    // Forward-compatible canonical wire material. request holds the complete
    // normalized create request; input_items is the resource exposed by
    // /responses/{id}/input_items; wire_snapshot preserves response fields the
    // typed core does not know yet. Renderers overlay typed authoritative state
    // on top of the snapshot rather than losing newer OpenAI fields.
    common_json                       input_items   = common_json::array();
    common_json                       wire_snapshot = common_json::object();
    // When an ancestor is evicted or explicitly deleted, its prompt-visible
    // context is folded into the direct child before removal. This keeps a
    // bounded store from making otherwise-live descendants impossible to
    // continue, without materializing the full chain in every response.
    std::optional<common_json>        detached_context;
    std::vector<response_output_item> output;
    response_usage                    usage;
    std::optional<response_error>     error;
    common_json                       incomplete_details = nullptr;
};

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_RESPONSE_TYPES_H
