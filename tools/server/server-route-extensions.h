#ifndef LLAMA_SERVER_ROUTE_EXTENSIONS_H
#define LLAMA_SERVER_ROUTE_EXTENSIONS_H

#include "server-http.h"
#include "server-responses.h"

#include <functional>

// Optional, statically linked extensions around stable llama-server seams.
// HTTP decorators receive the fully configured next handler and delegate
// requests outside their compatibility contract unchanged.
using server_http_handler_decorator =
    std::function<server_http_context::handler_t(server_http_context::handler_t next_handler)>;

struct server_route_extensions {
    server_responses_routes_factory responses;
    server_http_handler_decorator   v1_models;
};

#endif  // LLAMA_SERVER_ROUTE_EXTENSIONS_H
