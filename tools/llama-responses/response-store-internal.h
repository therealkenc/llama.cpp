#ifndef LLAMA_RESPONSES_RESPONSE_STORE_INTERNAL_H
#define LLAMA_RESPONSES_RESPONSE_STORE_INTERNAL_H

#include "response-types.h"

namespace llama_responses::response_store_detail {

bool        valid_state(const response_state & state);
common_json detached_context(const response_state & state);

}  // namespace llama_responses::response_store_detail

#endif  // LLAMA_RESPONSES_RESPONSE_STORE_INTERNAL_H
