#ifndef LLAMA_RESPONSES_GENERATION_H
#define LLAMA_RESPONSES_GENERATION_H

#include "json.h"
#include "response-types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace llama_responses {

// Input accepted by the model-runtime seam after public Responses request
// normalization. `prompt` and `parameters` describe model execution, not an
// HTTP or OpenAI response envelope. The llama-server adapter may initially
// carry its existing parsed prompt JSON here while the typed lowering grows.
struct generation_request {
    std::string model;
    common_json prompt     = nullptr;
    common_json parameters = common_json::object();
};

struct generation_started {};

struct generation_progress {};

struct generation_text_delta {
    std::string delta;
};

struct generation_reasoning_delta {
    std::string delta;
};

enum class generation_tool_kind {
    function,
    custom,
    local_shell,
    client_tool_search,
};

struct generation_tool_call_started {
    std::size_t          index = 0;
    generation_tool_kind kind  = generation_tool_kind::function;
    std::string          name;
    // The parser's call identity is optional. The Responses state machine
    // normalizes it into a call_* id independently from the output item id.
    std::string          upstream_call_id;
    // Namespace containers are flattened for the model chat template. The
    // adapter restores the public namespace on the generated output item.
    std::string          namespace_name;
};

struct generation_tool_call_delta {
    std::size_t index = 0;
    std::string delta;
};

struct generation_tool_call_reconciliation {
    std::size_t index = 0;
    std::string value;
};

// Authoritative final parser state. Streaming deltas remain append-only, but
// this update corrects the terminal response snapshot if the final parser pass
// removed a partial stop word or normalized a wrapped custom-tool value.
struct generation_message_reconciliation {
    std::string                                      reasoning;
    std::string                                      text;
    std::vector<generation_tool_call_reconciliation> tools;
};

struct generation_usage_update {
    response_usage usage;
};

struct generation_completed {
    response_usage usage;
    std::uint64_t  completed_at = 0;
};

struct generation_incomplete {
    response_usage usage;
    std::string    reason;
};

struct generation_failed {
    response_error                error;
    std::optional<response_usage> usage;
};

struct generation_cancelled {
    std::optional<response_usage> usage;
};

using generation_update = std::variant<generation_started,
                                       generation_progress,
                                       generation_text_delta,
                                       generation_reasoning_delta,
                                       generation_tool_call_started,
                                       generation_tool_call_delta,
                                       generation_message_reconciliation,
                                       generation_usage_update,
                                       generation_completed,
                                       generation_incomplete,
                                       generation_failed,
                                       generation_cancelled>;

bool generation_update_is_terminal(const generation_update & update) noexcept;

// Pull interface implemented by llama-server's existing response reader and
// by the deterministic fake below. Cancellation is a request to the producer;
// the producer reports the acknowledged terminal state as generation_cancelled.
class generation_session {
  public:
    virtual ~generation_session() = default;

    virtual std::optional<generation_update> next()                            = 0;
    virtual void                             request_cancel() noexcept         = 0;
    virtual bool                             cancel_requested() const noexcept = 0;
};

class generation_port {
  public:
    virtual ~generation_port() = default;

    virtual std::unique_ptr<generation_session> start(const generation_request & request) = 0;
};

// A repeatable fake for state-machine, route, and cancellation tests. Each
// start() receives its own cursor over the immutable script. If cancellation
// wins before a scripted terminal update, the session emits one cancelled
// update and then ends.
class scripted_generation_port final : public generation_port {
  public:
    explicit scripted_generation_port(std::vector<generation_update> script);

    std::unique_ptr<generation_session> start(const generation_request & request) override;

  private:
    std::vector<generation_update> script;
};

enum class generation_item_kind {
    reasoning,
    message,
    function_call,
    custom_tool_call,
    local_shell_call,
    tool_search_call,
};

// Identity creation is injected so production can use its preferred random
// source while tests remain exact. The state machine, rather than the model
// parser or HTTP renderer, decides when identities are allocated.
class generation_id_source {
  public:
    virtual ~generation_id_source() = default;

    virtual response_id next_response_id()                                 = 0;
    virtual item_id     next_item_id(generation_item_kind kind)            = 0;
    virtual call_id     next_call_id(const std::string & upstream_call_id) = 0;
};

// Counter-based source suitable for deterministic fixtures and for production
// when `namespace_value` is a process-unique random seed supplied by the host.
class counter_generation_id_source final : public generation_id_source {
  public:
    explicit counter_generation_id_source(std::string namespace_value);

    response_id next_response_id() override;
    item_id     next_item_id(generation_item_kind kind) override;
    call_id     next_call_id(const std::string & upstream_call_id) override;

  private:
    std::string next_value(const char * prefix, std::uint64_t & counter);

    std::mutex    mutex;
    std::string   namespace_value;
    std::uint64_t response_counter    = 0;
    std::uint64_t reasoning_counter   = 0;
    std::uint64_t message_counter     = 0;
    std::uint64_t function_counter    = 0;
    std::uint64_t custom_counter      = 0;
    std::uint64_t local_shell_counter = 0;
    std::uint64_t tool_search_counter = 0;
    std::uint64_t call_counter        = 0;
};

struct generation_response_context {
    std::string                model;
    common_json                request       = common_json::object();
    // Only this create request's direct input contribution. The separately
    // lowered server_generation_input already owns the materialized inference
    // context and must not leak that expanded prompt into durable state.
    common_json                input_items   = common_json::array();
    common_json                wire_snapshot = common_json::object();
    std::uint64_t              created_at    = 0;
    std::optional<response_id> previous_response;
};

// Render a typed event as the JSON object carried in an SSE `data:` field.
// HTTP framing deliberately remains outside the protocol state machine.
common_json render_generation_event(const response_event & event);

// One owner for response/item/call identity, output assembly, event ordering,
// terminal lifecycle, synchronous snapshots, and SSE event projections.
class native_response_state_machine final {
  public:
    native_response_state_machine(generation_response_context context, generation_id_source & ids);
    ~native_response_state_machine();

    native_response_state_machine(const native_response_state_machine &)             = delete;
    native_response_state_machine & operator=(const native_response_state_machine &) = delete;
    native_response_state_machine(native_response_state_machine &&) noexcept;
    native_response_state_machine & operator=(native_response_state_machine &&) noexcept;

    // start() is idempotent. apply() implicitly starts the response if needed
    // and returns only the events produced by that update.
    std::vector<response_event> start();
    std::vector<response_event> apply(const generation_update & update);

    // `active_state()` is the cheap generation head. During generation, the
    // accumulated text/tool values live in typed state-machine buffers and are
    // deliberately not copied into this JSON-shaped view after every delta.
    // Call materialized_state() or snapshot() at an explicit read boundary.
    const response_state & active_state() const noexcept;
    response_state         materialized_state() const;
    common_json            snapshot() const;

    // A failed apply() can have emitted a valid event prefix before detecting
    // an invalid later transition. Drain that suffix so the caller can publish
    // it immediately before the terminal failure events. Successful apply()
    // calls drain their own staging buffer in the returned vector.
    std::vector<response_event> take_pending_events() noexcept;
    bool                        terminal() const noexcept;

  private:
    class impl;
    std::unique_ptr<impl> implementation;
};

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_GENERATION_H
