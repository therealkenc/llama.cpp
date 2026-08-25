#ifndef LLAMA_RESPONSES_RESPONSE_SERVICE_H
#define LLAMA_RESPONSES_RESPONSE_SERVICE_H

#include "protocol-codec.h"
#include "response-store.h"

#include <cstdint>

namespace llama_responses {

enum class resource_result_kind {
    ok,
    not_found,
    invalid_request,
    conflict,
};

struct resource_result {
    resource_result_kind kind = resource_result_kind::ok;
    common_json          body = common_json::object();

    bool ok() const noexcept { return kind == resource_result_kind::ok; }
};

enum class completed_payload_kind {
    synchronous,
    sse_event,
};

// Pure route-facing service. It performs resource lookup and maps store/codec
// outcomes into protocol bodies, while leaving status-code selection, request
// parsing, and SSE framing to llama-server's route wrapper.
class response_resource_service {
  public:
    explicit response_resource_service(response_store & store);

    resource_result retrieve(const response_id & id) const;
    resource_result erase(const response_id & id);
    resource_result list_input_items(const response_id & id, const input_item_page_options & options = {}) const;
    resource_result completed_payload(const response_id &    id,
                                      completed_payload_kind kind,
                                      std::uint64_t          sequence_number = 0) const;

  private:
    response_store & store;
};

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_RESPONSE_SERVICE_H
