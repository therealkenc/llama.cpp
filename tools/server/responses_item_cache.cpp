#include "responses_item_cache.h"

#include <nlohmann/json.hpp>

using json_t = responses_item_cache::json_t;

void responses_item_cache::put(const std::string & id, const json_t & item) {
    if (id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu);

    auto it = by_id.find(id);
    if (it != by_id.end()) {
        it->second->item = item;
        lru.splice(lru.begin(), lru, it->second);
        return;
    }

    lru.push_front(entry{id, item});
    by_id.emplace(id, lru.begin());

    if (lru.size() > CAPACITY) {
        const auto & evict = lru.back();
        by_id.erase(evict.id);
        lru.pop_back();
    }
}

bool responses_item_cache::get(const std::string & id, json_t & out) {
    if (id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu);

    auto it = by_id.find(id);
    if (it == by_id.end()) {
        return false;
    }
    lru.splice(lru.begin(), lru, it->second);
    out = it->second->item;
    return true;
}
