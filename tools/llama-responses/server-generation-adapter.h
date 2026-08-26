#ifndef LLAMA_RESPONSES_SERVER_GENERATION_ADAPTER_H
#define LLAMA_RESPONSES_SERVER_GENERATION_ADAPTER_H

#include "generation.h"
#include "json.h"
#include "response-store.h"
#include "response-types.h"
#include "server-generation.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace llama_responses {

// Sidecar-owned projection of llama-server's protocol-neutral reader updates.
// The generic server retains prompt construction, MTMD, slots, sampling,
// cancellation, and transport; this sink owns Responses identity, state,
// durable checkpoints, synchronous JSON, and SSE event bytes.
class native_server_generation_sink final : public server_generation_sink {
  public:
    native_server_generation_sink(generation_response_context                  context,
                                  std::string                                  id_namespace,
                                  bool                                         stream,
                                  response_store *                             store          = nullptr,
                                  std::unordered_map<std::string, common_json> tool_metadata  = {},
                                  bool                                         journal_events = false);
    ~native_server_generation_sink() override;

    native_server_generation_sink(const native_server_generation_sink &)             = delete;
    native_server_generation_sink & operator=(const native_server_generation_sink &) = delete;

    std::string accept(const server_generation_update & update) override;
    common_json snapshot() const override;
    bool        cancel_requested() const noexcept override;

    void request_cancel() noexcept;
    bool wait_for_terminal(std::uint64_t timeout_ms) const;

    response_id    id() const;
    response_state state() const;
    bool           terminal() const;
    bool           storage_failed() const;
    std::string    storage_error() const;

    // Used when llama-server reports a pre-stream HTTP error after recording a
    // neutral failed update. Such a request did not create an API resource.
    void discard_persisted_state() noexcept;

  private:
    class impl;
    std::unique_ptr<impl> implementation;
};

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_SERVER_GENERATION_ADAPTER_H
