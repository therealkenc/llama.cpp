#ifndef LLAMA_RESPONSES_RESPONSE_STORE_H
#define LLAMA_RESPONSES_RESPONSE_STORE_H

#include "json.h"
#include "response-types.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace llama_responses {

enum class store_write_result {
    stored,
    already_exists,
    not_found,
    stale_revision,
    invalid_state,
    invalid_transition,
    item_id_conflict,
};

const char * store_write_result_name(store_write_result result) noexcept;

struct stored_response_item {
    response_id          owner;
    response_output_item item;
};

// The small, frequently changing part of a stored response. Event readers do
// not need to deserialize the request, input lineage, or output projection to
// decide whether a journal is complete or whether they should keep waiting.
struct response_store_head {
    response_id     id;
    response_status status               = response_status::in_progress;
    std::uint64_t   revision             = 0;
    std::uint64_t   next_sequence_number = 0;
    bool            event_journal        = false;
};

struct generation_store_write {
    store_write_result result   = store_write_result::invalid_state;
    std::uint64_t      revision = 0;
};

// One slice of a response's canonical event journal. `head` and `events` are
// observed while this store instance excludes its writers, so a subscriber can
// decide whether to wait or close without racing its terminal checkpoint.
struct response_event_page {
    response_store_head      head;
    std::vector<common_json> events;
    std::uint64_t            change_epoch = 0;
};

class response_store {
  public:
    virtual ~response_store() = default;

    // create() installs revision 1. replace() is compare-and-swap: the
    // supplied state's revision must match the currently stored revision.
    // Event rows, when present, commit in the same transaction as the state.
    virtual store_write_result     create(response_state state, const std::vector<common_json> & events = {})  = 0;
    virtual store_write_result     replace(response_state state, const std::vector<common_json> & events = {}) = 0;
    // Generation checkpoints use a compare-and-swap marker independent from
    // public visibility. Non-terminal checkpoints update only the response
    // head and append journal events. A terminal checkpoint additionally
    // installs the complete durable snapshot and item index once.
    virtual generation_store_write advance_generation(const response_state &           state,
                                                      std::uint64_t                    expected_generation_revision,
                                                      const std::vector<common_json> & events = {})            = 0;
    virtual std::optional<response_state>       find(const response_id & id) const                             = 0;
    virtual std::optional<stored_response_item> find_item(const item_id & id) const                            = 0;
    // These are public-target lookups: a missing or tombstoned target returns
    // nullopt. Their internal graph walk may cross tombstoned ancestors and a
    // migrated v4 legacy-lineage checkpoint.
    virtual std::optional<common_json>          materialize_input_items(const response_id & id) const          = 0;
    virtual std::optional<common_json>          materialize_continuation_context(const response_id & id) const = 0;
    virtual std::optional<response_event_page>  events_after(
        const response_id &                  id,
        const std::optional<std::uint64_t> & starting_after = std::nullopt) const                           = 0;
    virtual bool        wait_for_event_change(std::uint64_t observed_epoch, std::uint64_t timeout_ms) const = 0;
    // erase() removes public visibility but retains immutable lineage. The
    // HTTP layer enforces its active-resource delete policy; the store also
    // supports internal fail-stop cleanup of an active resource.
    virtual bool        erase(const response_id & id)                                                       = 0;
    virtual std::size_t size() const                                                                        = 0;
};

// Thread-safe process-local storage. max_entries limits visible resources, not
// retained immutable lineage: capacity pressure soft-deletes the oldest
// terminal visible response, while tombstones remain available to descendants.
// No protocol code has to know which storage strategy owns lineage or item
// references.
class in_memory_response_store final : public response_store {
  public:
    explicit in_memory_response_store(std::size_t max_entries = 4096);

    store_write_result            create(response_state state, const std::vector<common_json> & events = {}) override;
    store_write_result            replace(response_state state, const std::vector<common_json> & events = {}) override;
    generation_store_write        advance_generation(const response_state &           state,
                                                     std::uint64_t                    expected_generation_revision,
                                                     const std::vector<common_json> & events = {}) override;
    std::optional<response_state> find(const response_id & id) const override;
    std::optional<stored_response_item> find_item(const item_id & id) const override;
    std::optional<common_json>          materialize_input_items(const response_id & id) const override;
    std::optional<common_json>          materialize_continuation_context(const response_id & id) const override;
    std::optional<response_event_page>  events_after(
        const response_id &                  id,
        const std::optional<std::uint64_t> & starting_after = std::nullopt) const override;
    bool        wait_for_event_change(std::uint64_t observed_epoch, std::uint64_t timeout_ms) const override;
    bool        erase(const response_id & id) override;
    std::size_t size() const override;

  private:
    struct indexed_item {
        response_id          owner;
        response_output_item item;
    };

    bool                       item_ids_available(const response_state & state) const;
    void                       remove_item_index(const response_state & state);
    void                       add_item_index(const response_state & state);
    std::optional<common_json> materialize(const response_id & id, bool include_target_output) const;
    std::size_t                public_size() const;

    mutable std::mutex                              mutex;
    mutable std::condition_variable                 event_condition;
    std::map<response_id, response_state>           responses;
    std::map<item_id, indexed_item>                 items;
    std::map<response_id, std::vector<common_json>> event_journals;
    std::map<response_id, std::uint64_t>            generation_revisions;
    std::map<response_id, std::uint64_t>            tombstones;
    std::uint64_t                                   event_epoch = 0;
    std::size_t                                     max_entries;
    std::list<response_id>                          creation_order;
};

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_RESPONSE_STORE_H
