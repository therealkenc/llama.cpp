#ifndef LLAMA_RESPONSES_CODEX_MODELS_H
#define LLAMA_RESPONSES_CODEX_MODELS_H

#include "server-route-extensions.h"

namespace llama_responses {

// Decorates llama-server's fully configured model-list handler with the private
// JSON-bag catalog contract used by Codex clients. Requests outside a recognized
// Codex contract are delegated without inspecting the response.
server_http_handler_decorator make_codex_models_route_decorator();

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_CODEX_MODELS_H
