#ifndef LLAMA_RESPONSES_RESPONSE_STORE_INTERNAL_H
#define LLAMA_RESPONSES_RESPONSE_STORE_INTERNAL_H

#include "json.h"
#include "response-types.h"

#include <cstdint>
#include <vector>

namespace llama_responses::response_store_detail {

bool        valid_state(const response_state & state);
bool        event_journal_enabled(const response_state & state);
bool        valid_event_batch(bool                             journal_enabled,
                              response_status                  status,
                              std::uint64_t                    next_sequence_number,
                              std::uint64_t                    previous_next_sequence_number,
                              const std::vector<common_json> & events);
bool        valid_event_batch(const response_state &           state,
                              std::uint64_t                    previous_next_sequence_number,
                              const std::vector<common_json> & events);
common_json materialize_lineage(const std::vector<const response_state *> & lineage, bool include_target_output);
}  // namespace llama_responses::response_store_detail

#endif  // LLAMA_RESPONSES_RESPONSE_STORE_INTERNAL_H
