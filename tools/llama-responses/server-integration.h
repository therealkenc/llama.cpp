#ifndef LLAMA_RESPONSES_SERVER_INTEGRATION_H
#define LLAMA_RESPONSES_SERVER_INTEGRATION_H

#include "server-route-extensions.h"

namespace llama_responses {

// Creates the statically linked Responses implementation installed by the
// stock llama-server executable. The factory owns every Responses HTTP route
// and receives only typed model generation/counting services.
server_responses_routes_factory make_server_responses_routes_factory();

// Returns every route extension owned by this statically linked compatibility
// module. The Responses factory receives typed model services; ordinary HTTP
// decorators receive the configured stock handler as their next link.
server_route_extensions make_server_route_extensions();

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_SERVER_INTEGRATION_H
