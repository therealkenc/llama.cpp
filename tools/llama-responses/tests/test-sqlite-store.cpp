#include "json.h"
#include "response-store.h"
#include "response-types.h"
#include "sqlite-response-store.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

using namespace llama_responses;

namespace {

int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

class temporary_directory {
  public:
    temporary_directory() {
        const std::uint64_t nonce =
            static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() / ("llama-responses-sqlite-" + std::to_string(nonce));
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("failed to create temporary SQLite test directory");
        }
    }

    ~temporary_directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    temporary_directory(const temporary_directory &)             = delete;
    temporary_directory & operator=(const temporary_directory &) = delete;

    std::filesystem::path database_path() const { return path / "responses.sqlite3"; }

  private:
    std::filesystem::path path;
};

response_state make_state(const std::string & response_value,
                          const std::string & item_value,
                          const std::string & input_value) {
    response_state state;
    state.id                   = response_id(response_value);
    state.status               = response_status::in_progress;
    state.created_at           = 1724515200;
    state.next_sequence_number = 3;
    state.model                = "fixture-model";
    state.request              = {
        { "model", "fixture-model" },
        { "input", "remember this" },
    };
    state.metadata = {
        { "suite", "sqlite" }
    };

    common_json input = {
        { "id",      input_value                                                                       },
        { "type",    "message"                                                                         },
        { "role",    "user"                                                                            },
        { "content", common_json::array({ { { "type", "input_text" }, { "text", "remember this" } } }) },
    };
    state.input_items.push_back(input);
    state.continuation_input_items.push_back(std::move(input));
    state.wire_snapshot = {
        { "id",     response_value            },
        { "object", "response"                },
        { "future", { { "preserved", true } } },
    };

    response_output_item output;
    output.id    = item_id(item_value);
    output.type  = "message";
    output.value = {
        { "id",      item_value                                                                  },
        { "type",    "message"                                                                   },
        { "role",    "assistant"                                                                 },
        { "content", common_json::array({ { { "type", "output_text" }, { "text", "stored" } } }) },
    };
    state.output.push_back(std::move(output));
    state.usage.input_tokens  = 4;
    state.usage.output_tokens = 1;
    return state;
}

void materialize_after(const response_state & parent, response_state & child) {
    child.previous_response = parent.id;
    child.input_items       = parent.input_items;
    for (const response_output_item & output : parent.output) {
        common_json wire_item = output.value;
        wire_item["id"]       = output.id.str();
        wire_item["type"]     = output.type;
        child.input_items.push_back(std::move(wire_item));
    }
    for (const common_json & item : child.continuation_input_items) {
        child.input_items.push_back(item);
    }
}

void check_state_round_trip(const response_state & state) {
    CHECK(state.id == response_id("resp_sqlite_parent"));
    CHECK(state.revision == 1);
    CHECK(state.status == response_status::in_progress);
    CHECK(state.created_at == 1724515200);
    CHECK(state.next_sequence_number == 3);
    CHECK(state.model == "fixture-model");
    CHECK(state.request.at("input") == "remember this");
    CHECK(state.metadata.at("suite") == "sqlite");
    CHECK(state.input_items.size() == 1);
    CHECK(state.continuation_input_items.size() == 1);
    CHECK(state.continuation_input_items.at(0).at("id") == "msg_sqlite_parent_input");
    CHECK(state.wire_snapshot.at("future").at("preserved").get<bool>());
    CHECK(state.output.size() == 1);
    CHECK(state.output.at(0).id == item_id("msg_sqlite_parent_output"));
    CHECK(state.usage.total_tokens() == 5);
}

void test_persistence_and_compare_and_swap(const std::string & database_path) {
    response_state parent = make_state("resp_sqlite_parent", "msg_sqlite_parent_output", "msg_sqlite_parent_input");

    {
        sqlite_response_store store(database_path);
        CHECK(store.create(parent) == store_write_result::stored);
        CHECK(store.create(parent) == store_write_result::already_exists);
        CHECK(store.size() == 1);

        response_state conflict =
            make_state("resp_sqlite_conflict", "msg_sqlite_parent_output", "msg_sqlite_conflict_input");
        CHECK(store.create(conflict) == store_write_result::item_id_conflict);
        CHECK(store.size() == 1);
    }

    {
        sqlite_response_store store(database_path);
        CHECK(store.size() == 1);
        const auto reopened = store.find(parent.id);
        CHECK(reopened.has_value());
        if (!reopened) {
            return;
        }
        check_state_round_trip(*reopened);

        const auto item = store.find_item(item_id("msg_sqlite_parent_output"));
        CHECK(item.has_value());
        if (item) {
            CHECK(item->owner == parent.id);
            CHECK(item->item.value.at("content").at(0).at("text") == "stored");
        }

        response_state done      = *reopened;
        done.status              = response_status::completed;
        done.completed_at        = 1724515201;
        done.usage.output_tokens = 2;
        CHECK(store.replace(done) == store_write_result::stored);
        CHECK(store.replace(*reopened) == store_write_result::stale_revision);

        const auto completed = store.find(parent.id);
        CHECK(completed.has_value());
        if (completed) {
            CHECK(completed->revision == 2);
            CHECK(completed->status == response_status::completed);
            CHECK(completed->completed_at == std::optional<std::uint64_t>(1724515201));
            CHECK(completed->usage.output_tokens == 2);
        }
    }

    {
        sqlite_response_store store(database_path);
        const auto            completed = store.find(parent.id);
        CHECK(completed.has_value());
        if (completed) {
            response_state invalid = *completed;
            invalid.status         = response_status::in_progress;
            CHECK(store.replace(invalid) == store_write_result::invalid_transition);
        }
    }
}

void test_descendant_detachment_and_delete(const std::string & database_path) {
    response_state child    = make_state("resp_sqlite_child", "msg_sqlite_child_output", "msg_sqlite_child_input");
    child.previous_response = response_id("resp_sqlite_parent");

    {
        sqlite_response_store store(database_path);
        CHECK(store.create(child) == store_write_result::stored);
        CHECK(store.erase(response_id("resp_sqlite_parent")));
        CHECK(!store.find(response_id("resp_sqlite_parent")).has_value());
        CHECK(!store.find_item(item_id("msg_sqlite_parent_output")).has_value());

        const auto detached = store.find(child.id);
        CHECK(detached.has_value());
        if (detached) {
            CHECK(detached->revision == 2);
            CHECK(detached->detached_context.has_value());
            if (detached->detached_context) {
                CHECK(detached->detached_context->size() == 2);
                CHECK(detached->detached_context->at(0).at("id") == "msg_sqlite_parent_input");
                CHECK(detached->detached_context->at(1).at("id") == "msg_sqlite_parent_output");
            }
        }
    }

    {
        sqlite_response_store store(database_path);
        const auto            detached = store.find(child.id);
        CHECK(detached.has_value());
        CHECK(detached && detached->detached_context.has_value());
        CHECK(store.size() == 1);
        CHECK(store.erase(child.id));
        CHECK(!store.erase(child.id));
        CHECK(store.size() == 0);
    }
}

void test_middle_node_detachment_survives_restart(const std::string & database_path) {
    response_state root   = make_state("resp_sqlite_root", "msg_sqlite_root_output", "msg_sqlite_root_input");
    response_state middle = make_state("resp_sqlite_middle", "msg_sqlite_middle_output", "msg_sqlite_middle_input");
    response_state leaf   = make_state("resp_sqlite_leaf", "msg_sqlite_leaf_output", "msg_sqlite_leaf_input");
    materialize_after(root, middle);
    materialize_after(middle, leaf);

    {
        sqlite_response_store store(database_path);
        CHECK(store.create(root) == store_write_result::stored);
        CHECK(store.create(middle) == store_write_result::stored);
        CHECK(store.create(leaf) == store_write_result::stored);
        CHECK(store.erase(middle.id));
        CHECK(store.find(root.id).has_value());

        const auto detached = store.find(leaf.id);
        CHECK(detached && detached->detached_context.has_value());
        if (detached && detached->detached_context) {
            CHECK(detached->detached_context->size() == 4);
            CHECK(detached->detached_context->at(0).at("id") == "msg_sqlite_root_input");
            CHECK(detached->detached_context->at(1).at("id") == "msg_sqlite_root_output");
            CHECK(detached->detached_context->at(2).at("id") == "msg_sqlite_middle_input");
            CHECK(detached->detached_context->at(3).at("id") == "msg_sqlite_middle_output");
        }
    }

    {
        sqlite_response_store store(database_path);
        const auto            reopened_leaf = store.find(leaf.id);
        CHECK(reopened_leaf && reopened_leaf->detached_context.has_value());
        if (!reopened_leaf) {
            return;
        }

        response_state grandchild =
            make_state("resp_sqlite_grandchild", "msg_sqlite_grandchild_output", "msg_sqlite_grandchild_input");
        materialize_after(*reopened_leaf, grandchild);
        CHECK(store.create(grandchild) == store_write_result::stored);
        CHECK(store.erase(reopened_leaf->id));
    }

    {
        sqlite_response_store store(database_path);
        const auto            grandchild = store.find(response_id("resp_sqlite_grandchild"));
        CHECK(grandchild && grandchild->detached_context.has_value());
        if (grandchild && grandchild->detached_context) {
            CHECK(grandchild->detached_context->size() == 6);
            CHECK(grandchild->detached_context->at(0).at("id") == "msg_sqlite_root_input");
            CHECK(grandchild->detached_context->at(5).at("id") == "msg_sqlite_leaf_output");
        }
        CHECK(store.size() == 2);
    }
}

}  // namespace

int main() try {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();

    test_persistence_and_compare_and_swap(database_path);
    test_descendant_detachment_and_delete(database_path);
    test_middle_node_detachment_survives_restart(database_path);

    if (failures != 0) {
        std::cerr << failures << " SQLite response-store checks failed\n";
        return 1;
    }
    std::cout << "llama-responses SQLite store checks passed\n";
    return 0;
} catch (const std::exception & error) {
    std::cerr << "llama-responses SQLite store checks threw: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "llama-responses SQLite store checks threw an unknown exception\n";
    return 1;
}
