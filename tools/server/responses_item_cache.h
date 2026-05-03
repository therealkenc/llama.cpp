#pragma once

// LRU cache of OpenAI Responses output items, keyed by emitted item id
// (`fc_…`, `msg_…`, `rs_…`, etc.). Resolves `{type:"item_reference", id}`
// inputs that the @ai-sdk/openai Responses converter (and similar clients)
// emit when `store` is truthy.
//
// Process-singleton lifetime, owned by `server_context_impl`. Concurrency
// via `std::mutex` -- reads mutate LRU order, so `shared_mutex` would not
// help. Capacity is a fixed compile-time constant; promote to a CLI flag
// only if it ever bites.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

class responses_item_cache {
public:
    using json_t = nlohmann::ordered_json;

    static constexpr std::size_t CAPACITY = 4096;

    // Insert or refresh an item. Empty `id` is a no-op (defensive).
    void put(const std::string & id, const json_t & item);

    // Look up an item by id. Returns true on hit, copies into `out`,
    // and promotes to MRU. Returns false on miss; `out` is unchanged.
    bool get(const std::string & id, json_t & out);

private:
    struct entry {
        std::string id;
        json_t item;
    };

    using list_t = std::list<entry>;
    using iter_t = list_t::iterator;

    std::mutex mu;
    list_t lru;                                 // front = MRU
    std::unordered_map<std::string, iter_t> by_id;
};
