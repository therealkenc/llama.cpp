#ifndef LLAMA_RESPONSES_INPUT_LOWERING_H
#define LLAMA_RESPONSES_INPUT_LOWERING_H

#include "json.h"
#include "server-generation.h"

namespace llama_responses {

// Lower a validated Responses request whose input already contains resolved
// item references and materialized previous-response history. The result owns
// no HTTP or Responses output-envelope state; it is the typed model input
// consumed by llama-server's template/media/generation seam.
server_generation_input lower_responses_generation_input(const common_json & request);

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_INPUT_LOWERING_H
