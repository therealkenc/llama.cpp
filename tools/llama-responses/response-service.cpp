#include "response-service.h"

#include "protocol-codec.h"
#include "response-store.h"
#include "response-types.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace llama_responses {
namespace {

resource_result not_found(const response_id & id) {
    return { resource_result_kind::not_found, render_response_not_found(id) };
}

resource_result invalid_request(const std::string & message, const std::string & param) {
    return {
        resource_result_kind::invalid_request,
        render_error(message, "invalid_request_error", param, "invalid_parameter"),
    };
}

}  // namespace

response_resource_service::response_resource_service(response_store & store) : store(store) {}

resource_result response_resource_service::retrieve(const response_id & id) const {
    const auto state = store.find(id);
    return state ? resource_result{ resource_result_kind::ok, render_response(*state) } : not_found(id);
}

resource_result response_resource_service::erase(const response_id & id) {
    return store.erase(id) ? resource_result{ resource_result_kind::ok, render_deleted_response(id) } : not_found(id);
}

resource_result response_resource_service::list_input_items(const response_id &             id,
                                                            const input_item_page_options & options) const {
    const auto state = store.find(id);
    if (!state) {
        return not_found(id);
    }
    if (options.limit < 1 || options.limit > 100) {
        return invalid_request("input item page limit must be between 1 and 100", "limit");
    }
    try {
        return { resource_result_kind::ok, render_input_items_page(state->input_items, options) };
    } catch (const std::invalid_argument & error) {
        return invalid_request(error.what(), options.after ? "after" : "response_id");
    }
}

resource_result response_resource_service::completed_payload(const response_id &    id,
                                                             completed_payload_kind kind,
                                                             std::uint64_t          sequence_number) const {
    const auto state = store.find(id);
    if (!state) {
        return not_found(id);
    }
    if (!response_status_is_terminal(state->status)) {
        return {
            resource_result_kind::conflict,
            render_error("Response '" + id.str() + "' has not reached a terminal state.", "invalid_request_error",
                         std::string("response_id"), std::string("response_not_complete")),
        };
    }
    return {
        resource_result_kind::ok,
        kind == completed_payload_kind::sse_event ? render_terminal_event(*state, sequence_number) :
                                                    render_response(*state),
    };
}

}  // namespace llama_responses
