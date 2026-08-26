#ifndef LLAMA_SERVER_RESPONSES_H
#define LLAMA_SERVER_RESPONSES_H

#include "server-generation.h"
#include "server-http.h"

#include <cstdint>
#include <functional>
#include <memory>

// The only llama-server capabilities exposed to an alternate Responses API.
// Public protocol parsing and resource semantics stay in the route owner;
// llama-server retains templates, media loading, tokenization, slots, and
// generation. There is deliberately no legacy Responses handler in this
// contract, so an installed implementation cannot fall back to one.
struct server_generation_service {
    std::function<
        server_http_res_ptr(const server_http_req &, const server_generation_input &, server_generation_sink_ptr)>
                                                                  generate;
    std::function<std::uint64_t(const server_generation_input &)> count_input_tokens;
};

// Complete set of HTTP handlers owned by a Responses API implementation.
//
// `owner` is intentionally declared before the handlers so that it is destroyed
// after them. It lets a factory keep its implementation alive for as long as the
// handlers are registered without requiring each handler to capture that owner.
// `create` and `input_tokens` are required; the remaining operations are
// registered only when supplied.
struct server_responses_routes {
    std::shared_ptr<void> owner;

    // Called after HTTP intake stops and before server/backend objects are
    // destroyed. The generation queue may already have received its termination
    // signal, so alternate implementations must tolerate either ordering while
    // cancelling and joining owned background work here.
    std::function<void()> shutdown;

    server_http_context::handler_t create;
    server_http_context::handler_t input_tokens;
    server_http_context::handler_t retrieve;
    server_http_context::handler_t delete_response;
    server_http_context::handler_t cancel;
    server_http_context::handler_t compact;
    server_http_context::handler_t input_items;
};

// An installed implementation owns the entire Responses HTTP surface. The
// stock routes remain compiled and are selected only when no factory is
// installed.
using server_responses_routes_factory = std::function<server_responses_routes(server_generation_service)>;

#endif  // LLAMA_SERVER_RESPONSES_H
