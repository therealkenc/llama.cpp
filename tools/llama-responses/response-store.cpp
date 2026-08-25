#include "response-store.h"

#include "response-store-internal.h"
#include "response-types.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace llama_responses {

in_memory_response_store::in_memory_response_store(std::size_t max_entries) :
    max_entries(max_entries == 0 ? 1 : max_entries) {}

const char * store_write_result_name(store_write_result result) noexcept {
    switch (result) {
        case store_write_result::stored:
            return "stored";
        case store_write_result::already_exists:
            return "already_exists";
        case store_write_result::not_found:
            return "not_found";
        case store_write_result::stale_revision:
            return "stale_revision";
        case store_write_result::invalid_state:
            return "invalid_state";
        case store_write_result::invalid_transition:
            return "invalid_transition";
        case store_write_result::item_id_conflict:
            return "item_id_conflict";
    }
    return "invalid_state";
}

bool response_store_detail::valid_state(const response_state & state) {
    if (state.id.empty() || !state.request.is_object() || !state.input_items.is_array() ||
        !state.continuation_input_items.is_array() || !state.wire_snapshot.is_object()) {
        return false;
    }
    if (state.detached_context && !state.detached_context->is_array()) {
        return false;
    }

    std::set<item_id> seen;
    for (const common_json & item : state.input_items) {
        if (!item.is_object() || !item.contains("id") || !item.at("id").is_string()) {
            return false;
        }
        const item_id id(item.at("id").get<std::string>());
        if (id.empty() || !seen.insert(id).second) {
            return false;
        }
    }
    for (const response_output_item & item : state.output) {
        if (item.id.empty() || !seen.insert(item.id).second) {
            return false;
        }
    }
    std::set<item_id> continuation_seen;
    for (const common_json & item : state.continuation_input_items) {
        if (!item.is_object() || !item.contains("id") || !item.at("id").is_string()) {
            return false;
        }
        const item_id id(item.at("id").get<std::string>());
        if (id.empty() || !continuation_seen.insert(id).second) {
            return false;
        }
    }
    return true;
}

common_json response_store_detail::detached_context(const response_state & state) {
    // input_items is the fully materialized lineage observed through this
    // response: prior input/output followed by the current request input.
    // Building from only the local continuation would lose a still-attached
    // grandparent when an interior response is deleted first.
    common_json context = common_json::array();
    if (state.detached_context) {
        const bool already_materialized =
            state.input_items.size() >= state.detached_context->size() &&
            std::equal(state.detached_context->begin(), state.detached_context->end(), state.input_items.begin());
        if (!already_materialized) {
            context = *state.detached_context;
        }
    }
    for (const common_json & item : state.input_items) {
        context.push_back(item);
    }
    for (const response_output_item & item : state.output) {
        common_json wire_item = item.value.is_object() ? item.value : common_json::object();
        wire_item["id"]       = item.id.str();
        wire_item["type"]     = item.type;
        if (item.call) {
            wire_item["call_id"] = item.call->str();
        }
        context.push_back(std::move(wire_item));
    }
    return context;
}

bool in_memory_response_store::item_ids_available(const response_state & state) const {
    return std::all_of(state.output.begin(), state.output.end(), [this, &state](const response_output_item & item) {
        const auto found = items.find(item.id);
        return found == items.end() || found->second.owner == state.id;
    });
}

void in_memory_response_store::remove_item_index(const response_state & state) {
    for (const response_output_item & item : state.output) {
        const auto found = items.find(item.id);
        if (found != items.end() && found->second.owner == state.id) {
            items.erase(found);
        }
    }
}

void in_memory_response_store::add_item_index(const response_state & state) {
    for (const response_output_item & item : state.output) {
        items[item.id] = indexed_item{ state.id, item };
    }
}

void in_memory_response_store::detach_children(const response_state & state) {
    const common_json context = response_store_detail::detached_context(state);

    for (auto & entry : responses) {
        response_state & child = entry.second;
        if (child.previous_response && *child.previous_response == state.id) {
            child.detached_context = context;
            child.revision++;
        }
    }
}

store_write_result in_memory_response_store::create(response_state state) {
    if (!response_store_detail::valid_state(state)) {
        return store_write_result::invalid_state;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (responses.find(state.id) != responses.end()) {
        return store_write_result::already_exists;
    }
    if (!item_ids_available(state)) {
        return store_write_result::item_id_conflict;
    }

    while (responses.size() >= max_entries && !creation_order.empty()) {
        const response_id evicted_id = creation_order.front();
        creation_order.pop_front();
        creation_positions.erase(evicted_id);
        const auto evicted = responses.find(evicted_id);
        if (evicted != responses.end()) {
            detach_children(evicted->second);
            remove_item_index(evicted->second);
            responses.erase(evicted);
        }
    }

    state.revision = 1;
    add_item_index(state);
    const response_id id = state.id;
    responses.emplace(id, std::move(state));
    creation_order.push_back(id);
    creation_positions.emplace(id, std::prev(creation_order.end()));
    return store_write_result::stored;
}

store_write_result in_memory_response_store::replace(response_state state) {
    if (!response_store_detail::valid_state(state)) {
        return store_write_result::invalid_state;
    }

    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = responses.find(state.id);
    if (found == responses.end()) {
        return store_write_result::not_found;
    }
    if (state.revision != found->second.revision) {
        return store_write_result::stale_revision;
    }
    if (!response_status_can_transition(found->second.status, state.status)) {
        return store_write_result::invalid_transition;
    }
    if (!item_ids_available(state)) {
        return store_write_result::item_id_conflict;
    }

    if (!state.detached_context && found->second.detached_context) {
        state.detached_context = found->second.detached_context;
    }

    state.revision++;
    remove_item_index(found->second);
    add_item_index(state);
    found->second = std::move(state);
    return store_write_result::stored;
}

std::optional<response_state> in_memory_response_store::find(const response_id & id) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = responses.find(id);
    if (found == responses.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<stored_response_item> in_memory_response_store::find_item(const item_id & id) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = items.find(id);
    if (found == items.end()) {
        return std::nullopt;
    }
    return stored_response_item{ found->second.owner, found->second.item };
}

bool in_memory_response_store::erase(const response_id & id) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = responses.find(id);
    if (found == responses.end()) {
        return false;
    }
    detach_children(found->second);
    remove_item_index(found->second);
    const auto position = creation_positions.find(id);
    if (position != creation_positions.end()) {
        creation_order.erase(position->second);
        creation_positions.erase(position);
    }
    responses.erase(found);
    return true;
}

std::size_t in_memory_response_store::size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return responses.size();
}

}  // namespace llama_responses
