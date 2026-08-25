#ifndef LLAMA_SERVER_RESPONSES_H
#define LLAMA_SERVER_RESPONSES_H

#include "server-generation.h"
#include "server-http.h"

#include <functional>
#include <memory>

// Complete set of HTTP handlers owned by a Responses API implementation.
//
// `owner` is intentionally declared before the handlers so that it is destroyed
// after them. It lets a factory keep its implementation alive for as long as the
// handlers are registered without requiring each handler to capture that owner.
// `create` and `input_tokens` are required; the remaining operations are optional
// and receive a standard not-supported response when left empty.
struct server_responses_routes {
    std::shared_ptr<void> owner;

    // Internal generation seam. This is not registered as an HTTP route.
    // Decorators may use it to retain llama-server's prompt/MTMD/slot/reader
    // machinery while replacing only the protocol projection. It is optional
    // so an ordinary legacy bundle remains behaviorally unchanged.
    std::function<server_http_res_ptr(const server_http_req &, server_generation_sink_ptr)> generate;

    server_http_context::handler_t create;
    server_http_context::handler_t input_tokens;
    server_http_context::handler_t retrieve;
    server_http_context::handler_t delete_response;
    server_http_context::handler_t cancel;
    server_http_context::handler_t compact;
    server_http_context::handler_t input_items;
};

// The existing in-process llama-server routes are passed to the factory by value.
// An implementation can replace them wholesale, decorate individual handlers, or
// return them unchanged. The resulting bundle is installed atomically before the
// HTTP server starts accepting requests.
using server_responses_routes_factory = std::function<server_responses_routes(server_responses_routes legacy_routes)>;

#endif  // LLAMA_SERVER_RESPONSES_H
