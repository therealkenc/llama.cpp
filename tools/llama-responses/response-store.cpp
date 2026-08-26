#include "response-store.h"

#include "json.h"
#include "response-store-internal.h"
#include "response-types.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace llama_responses {
namespace {

common_json output_contribution(const response_output_item & item) {
    common_json result = item.value.is_object() ? item.value : common_json::object();
    result["id"]       = item.id.str();
    result["type"]     = item.type;
    if (item.call) {
        result["call_id"] = item.call->str();
    }
    return result;
}

void append_materialized_item(common_json & output, std::set<item_id> & seen, common_json item) {
    if (!item.is_object() || !item.contains("id") || !item.at("id").is_string()) {
        throw std::runtime_error("response lineage contains an item without a string id");
    }
    const item_id id(item.at("id").get<std::string>());
    if (id.empty() || !seen.insert(id).second) {
        throw std::runtime_error("response lineage contains an empty or duplicate item id");
    }
    output.push_back(std::move(item));
}

void append_node_input(common_json & output, std::set<item_id> & seen, const response_state & state) {
    for (const common_json & item : state.input_items) {
        append_materialized_item(output, seen, item);
    }
}

void append_node_output(common_json & output, std::set<item_id> & seen, const response_state & state) {
    for (const response_output_item & item : state.output) {
        append_materialized_item(output, seen, output_contribution(item));
    }
}

std::uint64_t deletion_time() {
    const std::time_t now = std::time(nullptr);
    return now < 0 ? 0U : static_cast<std::uint64_t>(now);
}

}  // namespace

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
        !state.wire_snapshot.is_object()) {
        return false;
    }
    if (state.legacy_lineage_checkpoint && !state.legacy_lineage_checkpoint->is_array()) {
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
    return true;
}

bool response_store_detail::event_journal_enabled(const response_state & state) {
    return state.request.contains("background") && state.request.at("background").is_boolean() &&
           state.request.at("background").get<bool>() && state.request.contains("stream") &&
           state.request.at("stream").is_boolean() && state.request.at("stream").get<bool>();
}

common_json response_store_detail::materialize_lineage(const std::vector<const response_state *> & lineage,
                                                       bool include_target_output) {
    if (lineage.empty()) {
        throw std::runtime_error("cannot materialize an empty response lineage");
    }

    common_json       result = common_json::array();
    std::set<item_id> seen;
    if (lineage.front()->legacy_lineage_checkpoint) {
        // The immediately enclosing engagement check proves value() is safe;
        // copying a potentially media-heavy checkpoint to appease path-insensitive analysis would be wasteful.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        const common_json & checkpoint = lineage.front()->legacy_lineage_checkpoint.value();
        if (!checkpoint.is_array()) {
            throw std::runtime_error("legacy response lineage checkpoint is not an array");
        }
        for (const common_json & item : checkpoint) {
            append_materialized_item(result, seen, item);
        }
    }

    for (std::size_t index = 0; index < lineage.size(); ++index) {
        append_node_input(result, seen, *lineage[index]);
        if (index + 1U < lineage.size() || include_target_output) {
            append_node_output(result, seen, *lineage[index]);
        }
    }
    return result;
}

bool response_store_detail::valid_event_batch(bool                             journal_enabled,
                                              response_status                  status,
                                              std::uint64_t                    next_sequence_number,
                                              std::uint64_t                    previous_next_sequence_number,
                                              const std::vector<common_json> & events) {
    if (!journal_enabled) {
        return events.empty();
    }

    std::uint64_t expected = previous_next_sequence_number;
    for (const common_json & event : events) {
        if (!event.is_object() || !event.contains("type") || !event.at("type").is_string() ||
            !event.contains("sequence_number") || !event.at("sequence_number").is_number_integer()) {
            return false;
        }
        try {
            if (event.at("sequence_number").get<std::uint64_t>() != expected) {
                return false;
            }
        } catch (const std::exception &) {
            return false;
        }
        if (expected == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        ++expected;
    }
    if (expected != next_sequence_number) {
        return false;
    }
    if (!response_status_is_terminal(status)) {
        return true;
    }
    if (events.empty()) {
        return false;
    }
    const std::string expected_terminal = std::string("response.") + response_status_name(status);
    return events.back().at("type") == expected_terminal;
}

bool response_store_detail::valid_event_batch(const response_state &           state,
                                              std::uint64_t                    previous_next_sequence_number,
                                              const std::vector<common_json> & events) {
    return valid_event_batch(event_journal_enabled(state), state.status, state.next_sequence_number,
                             previous_next_sequence_number, events);
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

store_write_result in_memory_response_store::create(response_state state, const std::vector<common_json> & events) {
    if (!response_store_detail::valid_state(state) || state.legacy_lineage_checkpoint ||
        !response_store_detail::valid_event_batch(state, 0, events)) {
        return store_write_result::invalid_state;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (responses.find(state.id) != responses.end()) {
        return store_write_result::already_exists;
    }
    if (state.previous_response) {
        const auto parent = responses.find(*state.previous_response);
        if (parent == responses.end() || !response_status_is_terminal(parent->second.status)) {
            return store_write_result::invalid_state;
        }
    }
    if (!item_ids_available(state)) {
        return store_write_result::item_id_conflict;
    }
    if (state.revision == std::numeric_limits<std::uint64_t>::max()) {
        return store_write_result::invalid_state;
    }

    while (public_size() >= max_entries) {
        const auto candidate =
            std::find_if(creation_order.begin(), creation_order.end(), [this](const response_id & id) {
                const auto response = responses.find(id);
                return response != responses.end() && tombstones.find(id) == tombstones.end() &&
                       response_status_is_terminal(response->second.status);
            });
        if (candidate == creation_order.end()) {
            return store_write_result::invalid_state;
        }
        tombstones.emplace(*candidate, deletion_time());
        ++event_epoch;
        event_condition.notify_all();
    }

    state.revision = 1;
    add_item_index(state);
    const response_id id = state.id;
    responses.emplace(id, std::move(state));
    generation_revisions.emplace(id, 1);
    if (!events.empty()) {
        event_journals.emplace(id, events);
        ++event_epoch;
        event_condition.notify_all();
    }
    creation_order.push_back(id);
    return store_write_result::stored;
}

store_write_result in_memory_response_store::replace(response_state state, const std::vector<common_json> & events) {
    if (!response_store_detail::valid_state(state)) {
        return store_write_result::invalid_state;
    }

    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = responses.find(state.id);
    if (found == responses.end() || tombstones.find(state.id) != tombstones.end()) {
        return store_write_result::not_found;
    }
    if (state.revision != found->second.revision) {
        return store_write_result::stale_revision;
    }
    if (!response_status_can_transition(found->second.status, state.status)) {
        return store_write_result::invalid_transition;
    }
    if (!response_store_detail::valid_event_batch(state, found->second.next_sequence_number, events)) {
        return store_write_result::invalid_state;
    }
    if (!item_ids_available(state)) {
        return store_write_result::item_id_conflict;
    }
    if (state.revision == std::numeric_limits<std::uint64_t>::max()) {
        return store_write_result::invalid_state;
    }

    // A response node's lineage contribution and edge are immutable after
    // create. Generic replacement may update its projection, but cannot
    // silently rewrite the history materialized by existing descendants.
    state.previous_response         = found->second.previous_response;
    state.input_items               = found->second.input_items;
    state.legacy_lineage_checkpoint = found->second.legacy_lineage_checkpoint;

    state.revision++;
    remove_item_index(found->second);
    add_item_index(state);
    found->second                      = std::move(state);
    generation_revisions[found->first] = found->second.revision;
    if (!events.empty()) {
        auto & journal = event_journals[found->first];
        journal.insert(journal.end(), events.begin(), events.end());
        ++event_epoch;
        event_condition.notify_all();
    }
    return store_write_result::stored;
}

generation_store_write in_memory_response_store::advance_generation(const response_state & state,
                                                                    std::uint64_t          expected_generation_revision,
                                                                    const std::vector<common_json> & events) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = responses.find(state.id);
    if (found == responses.end() || tombstones.find(state.id) != tombstones.end()) {
        return { store_write_result::not_found, 0 };
    }
    const auto generation = generation_revisions.find(state.id);
    if (generation == generation_revisions.end() || generation->second != expected_generation_revision) {
        return { store_write_result::stale_revision, found->second.revision };
    }
    if (!response_status_can_transition(found->second.status, state.status)) {
        return { store_write_result::invalid_transition, found->second.revision };
    }
    const bool journal_enabled = response_store_detail::event_journal_enabled(found->second);
    if (!response_store_detail::valid_event_batch(journal_enabled, state.status, state.next_sequence_number,
                                                  found->second.next_sequence_number, events)) {
        return { store_write_result::invalid_state, found->second.revision };
    }
    if (found->second.revision == std::numeric_limits<std::uint64_t>::max()) {
        return { store_write_result::invalid_state, found->second.revision };
    }
    if (response_status_is_terminal(state.status) && !response_store_detail::valid_state(state)) {
        return { store_write_result::invalid_state, found->second.revision };
    }
    if (response_status_is_terminal(state.status) && !item_ids_available(state)) {
        return { store_write_result::item_id_conflict, found->second.revision };
    }

    const std::uint64_t next_revision = found->second.revision + 1U;
    if (response_status_is_terminal(state.status)) {
        response_state next            = state;
        next.revision                  = next_revision;
        next.previous_response         = found->second.previous_response;
        next.input_items               = found->second.input_items;
        next.legacy_lineage_checkpoint = found->second.legacy_lineage_checkpoint;
        remove_item_index(found->second);
        add_item_index(next);
        found->second = std::move(next);
    } else {
        // The live generation sink owns the active output projection. Keep the
        // store's immutable/base snapshot and advance only its small head; the
        // complete projection is installed once at a terminal checkpoint.
        found->second.status               = state.status;
        found->second.completed_at         = state.completed_at;
        found->second.next_sequence_number = state.next_sequence_number;
        found->second.revision             = next_revision;
    }
    generation_revisions[state.id] = found->second.revision;
    if (!events.empty()) {
        auto & journal = event_journals[state.id];
        journal.insert(journal.end(), events.begin(), events.end());
        ++event_epoch;
        event_condition.notify_all();
    }
    return { store_write_result::stored, found->second.revision };
}

std::optional<response_state> in_memory_response_store::find(const response_id & id) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = responses.find(id);
    if (found == responses.end() || tombstones.find(id) != tombstones.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<stored_response_item> in_memory_response_store::find_item(const item_id & id) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = items.find(id);
    if (found == items.end() || tombstones.find(found->second.owner) != tombstones.end()) {
        return std::nullopt;
    }
    return stored_response_item{ found->second.owner, found->second.item };
}

std::optional<common_json> in_memory_response_store::materialize(const response_id & id,
                                                                 bool                include_target_output) const {
    const auto target = responses.find(id);
    if (target == responses.end() || tombstones.find(id) != tombstones.end()) {
        return std::nullopt;
    }

    std::vector<const response_state *> lineage;
    std::set<response_id>               visited;
    const response_state *              current = &target->second;
    for (;;) {
        if (!visited.insert(current->id).second) {
            throw std::runtime_error("response lineage contains a cycle");
        }
        lineage.push_back(current);
        if (current->legacy_lineage_checkpoint) {
            break;
        }
        if (!current->previous_response) {
            break;
        }
        const auto parent = responses.find(*current->previous_response);
        if (parent == responses.end()) {
            throw std::runtime_error("response lineage references a missing ancestor");
        }
        current = &parent->second;
    }

    std::reverse(lineage.begin(), lineage.end());
    return response_store_detail::materialize_lineage(lineage, include_target_output);
}

std::optional<common_json> in_memory_response_store::materialize_input_items(const response_id & id) const {
    std::lock_guard<std::mutex> lock(mutex);
    return materialize(id, false);
}

std::optional<common_json> in_memory_response_store::materialize_continuation_context(const response_id & id) const {
    std::lock_guard<std::mutex> lock(mutex);
    return materialize(id, true);
}

std::optional<response_event_page> in_memory_response_store::events_after(
    const response_id &                  id,
    const std::optional<std::uint64_t> & starting_after) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = responses.find(id);
    if (found == responses.end() || tombstones.find(id) != tombstones.end()) {
        return std::nullopt;
    }

    response_event_page page;
    page.head = {
        found->second.id,
        found->second.status,
        found->second.revision,
        found->second.next_sequence_number,
        response_store_detail::event_journal_enabled(found->second),
    };
    page.change_epoch  = event_epoch;
    const auto journal = event_journals.find(id);
    if (journal == event_journals.end()) {
        return page;
    }
    std::size_t first = 0;
    if (starting_after) {
        if (*starting_after == std::numeric_limits<std::uint64_t>::max()) {
            return page;
        }
        const std::uint64_t next = *starting_after + 1U;
        if (next >= journal->second.size()) {
            return page;
        }
        first = static_cast<std::size_t>(next);
    }
    const std::size_t count = std::min<std::size_t>(256, journal->second.size() - first);
    page.events.insert(page.events.end(), journal->second.begin() + static_cast<std::ptrdiff_t>(first),
                       journal->second.begin() + static_cast<std::ptrdiff_t>(first + count));
    return page;
}

bool in_memory_response_store::wait_for_event_change(std::uint64_t observed_epoch, std::uint64_t timeout_ms) const {
    std::unique_lock<std::mutex> lock(mutex);
    return event_condition.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                    [this, observed_epoch] { return event_epoch != observed_epoch; });
}

bool in_memory_response_store::erase(const response_id & id) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  found = responses.find(id);
    if (found == responses.end() || tombstones.find(id) != tombstones.end()) {
        return false;
    }
    tombstones.emplace(id, deletion_time());
    ++event_epoch;
    event_condition.notify_all();
    return true;
}

std::size_t in_memory_response_store::size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return public_size();
}

std::size_t in_memory_response_store::public_size() const {
    return responses.size() - tombstones.size();
}

}  // namespace llama_responses
