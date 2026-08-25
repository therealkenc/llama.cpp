#ifndef LLAMA_RESPONSES_SERVER_INTEGRATION_H
#define LLAMA_RESPONSES_SERVER_INTEGRATION_H

#include "server-route-extensions.h"

namespace llama_responses {

// Creates the statically linked Responses implementation installed by the
// stock llama-server executable. The factory decorates the existing in-process
// generation handlers; it never makes an HTTP call to Chat Completions.
server_responses_routes_factory make_server_responses_routes_factory();

// Returns every route decorator owned by this statically linked compatibility
// module. llama-server applies each decorator only after its legacy handler is
// fully configured.
server_route_extensions make_server_route_extensions();

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_SERVER_INTEGRATION_H
