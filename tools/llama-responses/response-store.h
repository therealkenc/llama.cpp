#ifndef LLAMA_RESPONSES_RESPONSE_STORE_H
#define LLAMA_RESPONSES_RESPONSE_STORE_H

#include "response-types.h"

#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <optional>

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

class response_store {
  public:
    virtual ~response_store() = default;

    // create() installs revision 1. replace() is compare-and-swap: the
    // supplied state's revision must match the currently stored revision.
    virtual store_write_result                  create(response_state state)        = 0;
    virtual store_write_result                  replace(response_state state)       = 0;
    virtual std::optional<response_state>       find(const response_id & id) const  = 0;
    virtual std::optional<stored_response_item> find_item(const item_id & id) const = 0;
    virtual bool                                erase(const response_id & id)       = 0;
    virtual std::size_t                         size() const                        = 0;
};

// Thread-safe process-local storage. It deliberately implements the same
// interface a SQLite/Redis/remote store would implement; no protocol code has
// to know which storage strategy owns previous_response_id/item_reference.
class in_memory_response_store final : public response_store {
  public:
    explicit in_memory_response_store(std::size_t max_entries = 4096);

    store_write_result                  create(response_state state) override;
    store_write_result                  replace(response_state state) override;
    std::optional<response_state>       find(const response_id & id) const override;
    std::optional<stored_response_item> find_item(const item_id & id) const override;
    bool                                erase(const response_id & id) override;
    std::size_t                         size() const override;

  private:
    struct indexed_item {
        response_id          owner;
        response_output_item item;
    };

    bool item_ids_available(const response_state & state) const;
    void remove_item_index(const response_state & state);
    void add_item_index(const response_state & state);
    void detach_children(const response_state & state);

    mutable std::mutex                                      mutex;
    std::map<response_id, response_state>                   responses;
    std::map<item_id, indexed_item>                         items;
    std::size_t                                             max_entries;
    std::list<response_id>                                  creation_order;
    std::map<response_id, std::list<response_id>::iterator> creation_positions;
};

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_RESPONSE_STORE_H
