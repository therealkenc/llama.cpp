#ifndef LLAMA_SERVER_GENERATION_H
#define LLAMA_SERVER_GENERATION_H

#include "chat.h"
#include "json.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

struct server_task_result;

// Protocol-neutral updates exposed by llama-server's existing completion
// reader. A consumer may project them into Responses, another API, telemetry,
// or a test transcript without taking ownership of slots or token generation.
struct server_generation_started {};

struct server_generation_progress {};

struct server_generation_message_deltas {
    std::vector<common_chat_msg_diff>            deltas;
    std::unordered_map<std::string, common_json> tool_metadata;
};

struct server_generation_usage {
    std::uint64_t input_tokens        = 0;
    std::uint64_t cached_input_tokens = 0;
    std::uint64_t output_tokens       = 0;
};

struct server_generation_error {
    std::string code;
    std::string message;
    std::string param;
};

struct server_generation_completed {
    server_generation_usage usage;
    std::uint64_t           completed_at = 0;
};

struct server_generation_incomplete {
    server_generation_usage usage;
    std::string             reason;
};

struct server_generation_failed {
    server_generation_error                error;
    std::optional<server_generation_usage> usage;
};

struct server_generation_cancelled {
    std::optional<server_generation_usage> usage;
};

using server_generation_update = std::variant<server_generation_started,
                                              server_generation_progress,
                                              server_generation_message_deltas,
                                              server_generation_completed,
                                              server_generation_incomplete,
                                              server_generation_failed,
                                              server_generation_cancelled>;

bool server_generation_update_is_terminal(const server_generation_update & update) noexcept;

// Translate one already-parsed completion result into neutral updates. The
// caller supplies include_started exactly once per generation. Message deltas
// always precede a terminal update produced from the same final result.
std::vector<server_generation_update> server_generation_updates_from_result(const server_task_result & result,
                                                                            bool                       include_started);

// Optional projection seam used only when an in-process route decorator asks
// llama-server for neutral generation. accept() returns opaque streaming bytes;
// llama-server preserves them verbatim, including any SSE framing selected by
// the consumer. snapshot() supplies the synchronous response body. The sink is
// called serially by the request's existing reader thread. Only
// cancel_requested() may be called while another HTTP handler sets an external
// cancellation flag, so implementations should make that query thread-safe.
class server_generation_sink {
  public:
    virtual ~server_generation_sink() = default;

    virtual std::string accept(const server_generation_update & update) = 0;
    virtual common_json snapshot() const                                = 0;

    virtual bool cancel_requested() const noexcept { return false; }
};

using server_generation_sink_ptr = std::shared_ptr<server_generation_sink>;

#endif  // LLAMA_SERVER_GENERATION_H
