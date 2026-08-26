#include "json.h"
#include "protocol-codec.h"
#include "response-store.h"
#include "response-types.h"
#include "sqlite-response-store.h"

#include <sqlite3.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

class raw_sqlite_database {
  public:
    explicit raw_sqlite_database(const std::string & path) {
        const int result = sqlite3_open(path.c_str(), &database);
        if (result != SQLITE_OK) {
            const std::string message = database != nullptr ? sqlite3_errmsg(database) : "no SQLite handle";
            if (database != nullptr) {
                sqlite3_close(database);
                database = nullptr;
            }
            throw std::runtime_error("opening SQLite test database failed: " + message);
        }
    }

    ~raw_sqlite_database() {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }

    raw_sqlite_database(const raw_sqlite_database &)             = delete;
    raw_sqlite_database & operator=(const raw_sqlite_database &) = delete;

    void execute(const std::string & sql) {
        char *    error  = nullptr;
        const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
        if (result == SQLITE_OK) {
            return;
        }
        const std::string message = error != nullptr ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        throw std::runtime_error("executing SQLite test statement failed: " + message);
    }

    std::uint64_t scalar_uint64(const std::string & sql) const {
        sqlite3_stmt * statement = nullptr;
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error("preparing SQLite scalar query failed: " + std::string(sqlite3_errmsg(database)));
        }
        const int result = sqlite3_step(statement);
        if (result != SQLITE_ROW || sqlite3_column_int64(statement, 0) < 0) {
            sqlite3_finalize(statement);
            throw std::runtime_error("SQLite scalar query did not return an unsigned integer");
        }
        const std::uint64_t value = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
        sqlite3_finalize(statement);
        return value;
    }

    std::string scalar_text(const std::string & sql) const {
        sqlite3_stmt * statement = nullptr;
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error("preparing SQLite text query failed: " + std::string(sqlite3_errmsg(database)));
        }
        if (sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("SQLite text query did not return a row");
        }
        const unsigned char * text  = sqlite3_column_text(statement, 0);
        const int             bytes = sqlite3_column_bytes(statement, 0);
        const std::string     value =
            text == nullptr ? std::string() : std::string(reinterpret_cast<const char *>(text), bytes);
        sqlite3_finalize(statement);
        return value;
    }

    sqlite3 * handle() const noexcept { return database; }

  private:
    sqlite3 * database = nullptr;
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
    state.input_items.push_back(std::move(input));
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

void link_after(const response_state & parent, response_state & child) {
    child.previous_response = parent.id;
}

response_state make_completed_state(const std::string & response_value,
                                    const std::string & item_value,
                                    const std::string & input_value) {
    response_state state = make_state(response_value, item_value, input_value);
    state.status         = response_status::completed;
    state.completed_at   = 1724515201;
    return state;
}

void check_state_round_trip(const response_state & state) {
    CHECK(state.id == response_id("resp_sqlite_parent"));
    CHECK(state.revision == 1);
    CHECK(state.status == response_status::in_progress);
    CHECK(state.created_at == 1724515200);
    CHECK(state.next_sequence_number == 3);
    CHECK(state.model == "fixture-model");
    CHECK(!state.request.contains("input"));
    CHECK(state.metadata.at("suite") == "sqlite");
    CHECK(state.input_items.size() == 1);
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

void test_sqlite_revision_ceiling_returns_invalid_state() {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();
    response_state      replace_state =
        make_state("resp_sqlite_replace_ceiling", "msg_sqlite_replace_ceiling", "msg_sqlite_replace_input");
    response_state advance_state =
        make_state("resp_sqlite_advance_ceiling", "msg_sqlite_advance_ceiling", "msg_sqlite_advance_input");
    advance_state.output.clear();

    sqlite_response_store store(database_path);
    CHECK(store.create(replace_state) == store_write_result::stored);
    CHECK(store.create(advance_state) == store_write_result::stored);

    raw_sqlite_database raw(database_path);
    const std::string   maximum = std::to_string(std::numeric_limits<sqlite3_int64>::max());
    raw.execute("UPDATE responses SET revision=" + maximum +
                " WHERE id IN ('resp_sqlite_replace_ceiling', 'resp_sqlite_advance_ceiling');"
                "UPDATE response_heads SET revision=" +
                maximum + ", generation_revision=" + maximum +
                " WHERE response_id IN ('resp_sqlite_replace_ceiling', 'resp_sqlite_advance_ceiling');");

    const auto replace_current = store.find(replace_state.id);
    CHECK(replace_current.has_value());
    if (replace_current) {
        response_state completed = *replace_current;
        completed.status         = response_status::completed;
        completed.completed_at   = 1724515201;
        CHECK(store.replace(std::move(completed)) == store_write_result::invalid_state);
    }

    advance_state.status               = response_status::completed;
    advance_state.completed_at         = 1724515201;
    advance_state.next_sequence_number = 3;
    const generation_store_write advanced =
        store.advance_generation(advance_state, static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()));
    CHECK(advanced.result == store_write_result::invalid_state);
    CHECK(advanced.revision == static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()));
}

void test_restart_recovery_rejects_sqlite_integer_ceilings() {
    const std::string maximum = std::to_string(std::numeric_limits<sqlite3_int64>::max());
    {
        temporary_directory   directory;
        sqlite_response_store store(directory.database_path().string());
        response_state        active =
            make_state("resp_sqlite_recovery_revision", "msg_sqlite_recovery_revision", "msg_recovery_input");
        CHECK(store.create(active) == store_write_result::stored);

        raw_sqlite_database raw(directory.database_path().string());
        raw.execute("UPDATE responses SET revision=" + maximum +
                    " WHERE id='resp_sqlite_recovery_revision';"
                    "UPDATE response_heads SET revision=" +
                    maximum + ", generation_revision=" + maximum +
                    " WHERE response_id='resp_sqlite_recovery_revision';");
        try {
            static_cast<void>(store.fail_interrupted_responses());
            CHECK(false);
        } catch (const std::runtime_error & error) {
            CHECK(std::string(error.what()) == "unable to terminalize an interrupted response at maximum revision");
        }
    }
    {
        temporary_directory   directory;
        sqlite_response_store store(directory.database_path().string());
        response_state        active =
            make_state("resp_journal_recovery_sequence", "msg_journal_recovery_sequence", "msg_recovery_input");
        active.request["background"] = true;
        active.request["stream"]     = true;
        active.next_sequence_number  = 2;
        const common_json created    = {
            { "type",            "response.created" },
            { "sequence_number", 0                  }
        };
        const common_json in_progress = {
            { "type",            "response.in_progress" },
            { "sequence_number", 1                      }
        };
        CHECK(store.create(active, { created, in_progress }) == store_write_result::stored);

        raw_sqlite_database raw(directory.database_path().string());
        raw.execute("UPDATE responses SET next_sequence_number=" + maximum +
                    " WHERE id='resp_journal_recovery_sequence';"
                    "UPDATE response_heads SET next_sequence_number=" +
                    maximum + " WHERE response_id='resp_journal_recovery_sequence';");
        try {
            static_cast<void>(store.fail_interrupted_responses());
            CHECK(false);
        } catch (const std::runtime_error & error) {
            CHECK(std::string(error.what()) == "unable to append a restart failure at maximum event sequence");
        }
    }
}

void check_item_ids(const std::optional<common_json> & items, const std::vector<std::string> & expected) {
    CHECK(items.has_value());
    if (!items) {
        return;
    }
    CHECK(items->size() == expected.size());
    if (items->size() != expected.size()) {
        return;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(items->at(index).at("id") == expected[index]);
    }
}

void test_immutable_graph_branches_tombstones_and_restart() {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();
    response_state root = make_completed_state("resp_sqlite_root", "msg_sqlite_root_output", "msg_sqlite_root_input");
    response_state middle =
        make_completed_state("resp_sqlite_middle", "msg_sqlite_middle_output", "msg_sqlite_middle_input");
    response_state leaf = make_completed_state("resp_sqlite_leaf", "msg_sqlite_leaf_output", "msg_sqlite_leaf_input");
    response_state branch =
        make_completed_state("resp_sqlite_branch", "msg_sqlite_branch_output", "msg_sqlite_branch_input");
    link_after(root, middle);
    link_after(middle, leaf);
    link_after(root, branch);

    {
        sqlite_response_store store(database_path);
        CHECK(store.create(root) == store_write_result::stored);
        CHECK(store.create(middle) == store_write_result::stored);
        CHECK(store.create(leaf) == store_write_result::stored);
        CHECK(store.create(branch) == store_write_result::stored);
        check_item_ids(store.materialize_input_items(leaf.id),
                       { "msg_sqlite_root_input", "msg_sqlite_root_output", "msg_sqlite_middle_input",
                         "msg_sqlite_middle_output", "msg_sqlite_leaf_input" });
        check_item_ids(store.materialize_continuation_context(branch.id),
                       { "msg_sqlite_root_input", "msg_sqlite_root_output", "msg_sqlite_branch_input",
                         "msg_sqlite_branch_output" });

        CHECK(store.erase(root.id));
        CHECK(store.erase(middle.id));
        CHECK(!store.find(root.id).has_value());
        CHECK(!store.find_item(item_id("msg_sqlite_root_output")).has_value());
        CHECK(!store.materialize_input_items(root.id).has_value());
        const auto retained_leaf = store.find(leaf.id);
        CHECK(retained_leaf && retained_leaf->revision == 1U);
        check_item_ids(store.materialize_input_items(leaf.id),
                       { "msg_sqlite_root_input", "msg_sqlite_root_output", "msg_sqlite_middle_input",
                         "msg_sqlite_middle_output", "msg_sqlite_leaf_input" });
        CHECK(store.size() == 2U);
    }

    {
        sqlite_response_store store(database_path);
        check_item_ids(store.materialize_continuation_context(leaf.id),
                       { "msg_sqlite_root_input", "msg_sqlite_root_output", "msg_sqlite_middle_input",
                         "msg_sqlite_middle_output", "msg_sqlite_leaf_input", "msg_sqlite_leaf_output" });

        // Simulate delete after request preparation but before child commit.
        // The public parent disappears, while the already-prepared child may
        // still commit its immutable edge to the internal tombstone.
        response_state grandchild = make_completed_state("resp_sqlite_grandchild", "msg_sqlite_grandchild_output",
                                                         "msg_sqlite_grandchild_input");
        link_after(leaf, grandchild);
        CHECK(store.erase(leaf.id));
        CHECK(store.create(grandchild) == store_write_result::stored);
        check_item_ids(
            store.materialize_input_items(grandchild.id),
            { "msg_sqlite_root_input", "msg_sqlite_root_output", "msg_sqlite_middle_input", "msg_sqlite_middle_output",
              "msg_sqlite_leaf_input", "msg_sqlite_leaf_output", "msg_sqlite_grandchild_input" });
    }

    {
        sqlite_response_store store(database_path);
        CHECK(!store.find(root.id).has_value());
        CHECK(!store.find(middle.id).has_value());
        CHECK(!store.find(leaf.id).has_value());
        check_item_ids(store.materialize_continuation_context(response_id("resp_sqlite_grandchild")),
                       { "msg_sqlite_root_input", "msg_sqlite_root_output", "msg_sqlite_middle_input",
                         "msg_sqlite_middle_output", "msg_sqlite_leaf_input", "msg_sqlite_leaf_output",
                         "msg_sqlite_grandchild_input", "msg_sqlite_grandchild_output" });
        CHECK(store.size() == 2U);
    }
}

void test_multimodal_tool_lineage_survives_restart_and_parent_delete() {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();
    response_state      parent =
        make_completed_state("resp_sqlite_media_parent", "fc_sqlite_assistant", "msg_sqlite_media_input");
    const common_json rich_input = common_json::array({
        {
         { "id", "fc_sqlite_media" },
         { "type", "function_call" },
         { "call_id", "call_sqlite_media" },
         { "name", "inspect" },
         { "arguments", "{}" },
         },
        {
         { "id", "fco_sqlite_media" },
         { "type", "function_call_output" },
         { "call_id", "call_sqlite_media" },
         { "output", common_json::array({
                            { { "type", "input_text" }, { "text", "before" } },
                            { { "type", "input_image" }, { "image_url", "data:image/png;base64,QUFB" } },
                            { { "type", "input_file" }, { "filename", "notes.txt" }, { "file_data", "bm90ZXM=" } },
                            { { "type", "input_text" }, { "text", "after" } },
                        }) },
         },
    });
    parent.input_items           = rich_input;
    parent.output.at(0).type     = "function_call";
    parent.output.at(0).call     = call_id("call_sqlite_assistant");
    parent.output.at(0).value    = {
        { "id",        "fc_sqlite_assistant"    },
        { "type",      "function_call"          },
        { "call_id",   "call_sqlite_assistant"  },
        { "name",      "summarize"              },
        { "arguments", "{\"format\":\"short\"}" },
    };

    {
        sqlite_response_store store(database_path);
        CHECK(store.create(parent) == store_write_result::stored);
    }

    response_state child = make_completed_state("resp_sqlite_media_child", "msg_sqlite_media_child_output",
                                                "msg_sqlite_media_child_input");
    {
        sqlite_response_store store(database_path);
        const auto            reopened_parent = store.find(parent.id);
        CHECK(reopened_parent.has_value());
        if (!reopened_parent) {
            return;
        }
        CHECK(reopened_parent->input_items == rich_input);
        link_after(*reopened_parent, child);
        CHECK(store.create(child) == store_write_result::stored);
        CHECK(store.erase(parent.id));
    }

    {
        sqlite_response_store store(database_path);
        const auto            context = store.materialize_input_items(child.id);
        CHECK(context.has_value());
        if (!context) {
            return;
        }
        CHECK(context->size() == 4U);
        CHECK(context->at(0) == rich_input.at(0));
        CHECK(context->at(1) == rich_input.at(1));
        CHECK(context->at(1).at("output").at(1).at("image_url") == "data:image/png;base64,QUFB");
        CHECK(context->at(1).at("output").at(2).at("file_data") == "bm90ZXM=");
        CHECK(context->at(2).at("id") == "fc_sqlite_assistant");
        CHECK(context->at(2).at("call_id") == "call_sqlite_assistant");
        CHECK(context->at(2).at("arguments") == "{\"format\":\"short\"}");
        CHECK(context->at(3).at("id") == "msg_sqlite_media_child_input");
    }
}

void test_interrupted_responses_fail_on_restart() {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();

    response_state queued = make_state("resp_sqlite_queued", "msg_sqlite_queued", "msg_sqlite_queued_input");
    queued.status         = response_status::queued;
    response_state active = make_state("resp_sqlite_active", "msg_sqlite_active", "msg_sqlite_active_input");
    response_state completed =
        make_state("resp_sqlite_completed", "msg_sqlite_completed", "msg_sqlite_completed_input");
    completed.status       = response_status::completed;
    completed.completed_at = 1724515201;

    sqlite_response_store store(database_path);
    CHECK(store.create(queued) == store_write_result::stored);
    CHECK(store.create(active) == store_write_result::stored);
    CHECK(store.create(completed) == store_write_result::stored);
    CHECK(store.fail_interrupted_responses() == 2U);

    for (const response_id & id : { queued.id, active.id }) {
        const auto failed = store.find(id);
        CHECK(failed.has_value());
        if (failed) {
            CHECK(failed->revision == 2U);
            CHECK(failed->status == response_status::failed);
            CHECK(!failed->completed_at.has_value());
            CHECK(failed->error.has_value());
            CHECK(failed->error && failed->error->code == "server_restarted");
        }
    }

    const auto unchanged = store.find(completed.id);
    CHECK(unchanged.has_value());
    if (unchanged) {
        CHECK(unchanged->revision == 1U);
        CHECK(unchanged->status == response_status::completed);
    }
    CHECK(store.fail_interrupted_responses() == 0U);
}

common_json journal_event(std::uint64_t sequence_number, const std::string & type) {
    return {
        { "type",            type            },
        { "sequence_number", sequence_number },
    };
}

response_state make_journal_state(const std::string & suffix) {
    response_state state = make_state("resp_journal_" + suffix, "msg_journal_" + suffix, "msg_journal_input_" + suffix);
    state.request["background"] = true;
    state.request["stream"]     = true;
    state.next_sequence_number  = 2;
    return state;
}

void test_incremental_generation_head_and_terminal_snapshot() {
    temporary_directory directory;
    response_state      active = make_journal_state("incremental");
    active.output.clear();
    const std::vector<common_json> initial = {
        journal_event(0, "response.created"),
        journal_event(1, "response.in_progress"),
    };

    sqlite_response_store store(directory.database_path().string());
    CHECK(store.create(active, initial) == store_write_result::stored);

    response_output_item output;
    output.id    = item_id("msg_incremental_output");
    output.type  = "message";
    output.value = {
        { "id",      output.id.str()                                                              },
        { "type",    "message"                                                                    },
        { "role",    "assistant"                                                                  },
        { "content", common_json::array({ { { "type", "output_text" }, { "text", "partial" } } }) },
    };
    active.output.push_back(output);
    active.next_sequence_number = 3;
    const generation_store_write advanced =
        store.advance_generation(active, 1, { journal_event(2, "response.output_text.delta") });
    CHECK(advanced.result == store_write_result::stored);
    CHECK(advanced.revision == 2U);

    // A non-terminal checkpoint advances the tiny durable head but deliberately
    // leaves the initial snapshot and item index untouched. The active sink is
    // the same-process source for a live output projection.
    const auto durable_active = store.find(active.id);
    CHECK(durable_active && durable_active->revision == 2U);
    CHECK(durable_active && durable_active->next_sequence_number == 3U);
    CHECK(durable_active && durable_active->output.empty());
    CHECK(!store.find_item(output.id).has_value());

    active.status               = response_status::completed;
    active.completed_at         = 1724515201;
    active.next_sequence_number = 4;
    const generation_store_write completed =
        store.advance_generation(active, advanced.revision, { render_terminal_event(active, 3) });
    CHECK(completed.result == store_write_result::stored);
    CHECK(completed.revision == 3U);

    const auto durable_completed = store.find(active.id);
    CHECK(durable_completed && durable_completed->revision == 3U);
    CHECK(durable_completed && durable_completed->status == response_status::completed);
    CHECK(durable_completed && durable_completed->output.size() == 1U);
    CHECK(store.find_item(output.id).has_value());
    CHECK(store.advance_generation(active, advanced.revision).result == store_write_result::stale_revision);
}

void test_active_child_recovers_through_tombstoned_ancestor() {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();
    response_state      parent =
        make_completed_state("resp_incremental_parent", "msg_incremental_parent", "msg_parent_input");
    response_state child = make_journal_state("tombstoned_ancestor");
    child.output.clear();
    link_after(parent, child);

    {
        sqlite_response_store store(database_path);
        CHECK(store.create(parent) == store_write_result::stored);
        CHECK(store.create(child, { journal_event(0, "response.created"), journal_event(1, "response.in_progress") }) ==
              store_write_result::stored);
        CHECK(store.erase(parent.id));

        const auto retained = store.find(child.id);
        CHECK(retained && retained->revision == 1U);
        child.next_sequence_number = 3;
        const generation_store_write advanced =
            store.advance_generation(child, 1, { journal_event(2, "response.output_text.delta") });
        CHECK(advanced.result == store_write_result::stored);
        CHECK(advanced.revision == 2U);
    }

    {
        sqlite_response_store store(database_path);
        CHECK(store.fail_interrupted_responses() == 1U);
        const auto failed = store.events_after(child.id);
        CHECK(failed && failed->events.size() == 4U);
        CHECK(failed && failed->events.back().at("sequence_number") == 3);
        CHECK(failed && failed->events.back().at("type") == "response.failed");
        check_item_ids(store.materialize_input_items(child.id),
                       { "msg_parent_input", "msg_incremental_parent", "msg_journal_input_tombstoned_ancestor" });
    }
}

void test_event_journal_append_cursor_reopen_and_delete() {
    temporary_directory            directory;
    const std::string              database_path = directory.database_path().string();
    response_state                 active        = make_journal_state("round_trip");
    const std::vector<common_json> initial       = {
        journal_event(0, "response.created"),
        journal_event(1, "response.in_progress"),
    };

    {
        sqlite_response_store store(database_path);
        CHECK(store.create(active, initial) == store_write_result::stored);
        const auto stored = store.find(active.id);
        CHECK(stored.has_value());
        if (!stored) {
            return;
        }

        response_state completed               = *stored;
        completed.status                       = response_status::completed;
        completed.completed_at                 = 1724515201;
        completed.next_sequence_number         = 4;
        const std::vector<common_json> invalid = {
            journal_event(3, "response.output_text.delta"),
            render_terminal_event(completed, 3),
        };
        CHECK(store.replace(completed, invalid) == store_write_result::invalid_state);
        const auto unchanged = store.events_after(active.id);
        CHECK(unchanged && unchanged->events.size() == 2U);

        const std::vector<common_json> terminal = {
            journal_event(2, "response.output_text.delta"),
            render_terminal_event(completed, 3),
        };
        CHECK(store.replace(completed, terminal) == store_write_result::stored);
        const auto suffix = store.events_after(active.id, 0);
        CHECK(suffix && suffix->events.size() == 3U);
        if (suffix && suffix->events.size() == 3U) {
            CHECK(suffix->events.front().at("sequence_number") == 1);
            CHECK(suffix->events.back().at("sequence_number") == 3);
            CHECK(suffix->events.back().at("type") == "response.completed");
        }
    }

    {
        sqlite_response_store store(database_path);
        const auto            reopened = store.events_after(active.id);
        CHECK(reopened && reopened->events.size() == 4U);
        if (reopened) {
            for (std::size_t index = 0; index < reopened->events.size(); ++index) {
                CHECK(reopened->events[index].at("sequence_number") == index);
            }
            CHECK(reopened->head.status == response_status::completed);
        }
        const auto at_tail = store.events_after(active.id, 3);
        CHECK(at_tail && at_tail->events.empty());
        const auto future = store.events_after(active.id, 1000000);
        CHECK(future && future->events.empty());
        const auto maximum = store.events_after(active.id, std::numeric_limits<std::uint64_t>::max());
        CHECK(maximum && maximum->events.empty());
        CHECK(store.erase(active.id));
        CHECK(!store.events_after(active.id).has_value());
    }
}

void test_event_journal_active_gap_is_not_a_future_cursor() {
    temporary_directory directory;
    const std::string   database_path     = directory.database_path().string();
    response_state      active            = make_journal_state("active_gap");
    active.next_sequence_number           = 4;
    const std::vector<common_json> events = {
        journal_event(0, "response.created"),
        journal_event(1, "response.in_progress"),
        journal_event(2, "response.output_text.delta"),
        journal_event(3, "response.output_text.delta"),
    };

    sqlite_response_store store(database_path);
    CHECK(store.create(active, events) == store_write_result::stored);
    const auto at_tail = store.events_after(active.id, 3);
    CHECK(at_tail && at_tail->events.empty());
    const auto future = store.events_after(active.id, std::numeric_limits<std::uint64_t>::max());
    CHECK(future && future->events.empty());

    raw_sqlite_database raw(database_path);
    raw.execute("DELETE FROM response_events WHERE response_id='resp_journal_active_gap' AND sequence_number=3");
    bool detected_gap = false;
    try {
        static_cast<void>(store.events_after(active.id, 2));
    } catch (const std::runtime_error &) {
        detected_gap = true;
    }
    CHECK(detected_gap);
    const auto beyond_corrupt_tail = store.events_after(active.id, 1000);
    CHECK(beyond_corrupt_tail && beyond_corrupt_tail->events.empty());
}

void test_event_journal_pages_without_gaps_or_duplicates() {
    temporary_directory directory;
    response_state      active  = make_journal_state("pagination");
    active.next_sequence_number = 258;
    std::vector<common_json> events;
    events.reserve(258);
    for (std::uint64_t sequence_number = 0; sequence_number < 258; ++sequence_number) {
        events.push_back(journal_event(sequence_number, "response.output_text.delta"));
    }

    sqlite_response_store store(directory.database_path().string());
    CHECK(store.create(active, events) == store_write_result::stored);
    const auto first = store.events_after(active.id);
    CHECK(first && first->events.size() == 256U);
    if (!first || first->events.size() != 256U) {
        return;
    }
    CHECK(first->events.front().at("sequence_number") == 0);
    CHECK(first->events.back().at("sequence_number") == 255);

    const auto second = store.events_after(active.id, 255);
    CHECK(second && second->events.size() == 2U);
    if (second && second->events.size() == 2U) {
        CHECK(second->events.front().at("sequence_number") == 256);
        CHECK(second->events.back().at("sequence_number") == 257);
    }
}

void test_event_journal_restart_failure_is_terminal_and_idempotent() {
    temporary_directory            directory;
    const std::string              database_path = directory.database_path().string();
    response_state                 active        = make_journal_state("restart");
    const std::vector<common_json> initial       = {
        journal_event(0, "response.created"),
        journal_event(1, "response.in_progress"),
    };

    {
        sqlite_response_store store(database_path);
        CHECK(store.create(active, initial) == store_write_result::stored);
    }
    {
        sqlite_response_store store(database_path);
        CHECK(store.fail_interrupted_responses() == 1U);
        const auto failed = store.events_after(active.id);
        CHECK(failed && failed->events.size() == 3U);
        if (failed) {
            CHECK(failed->head.status == response_status::failed);
            CHECK(failed->head.next_sequence_number == 3U);
            CHECK(failed->events.back().at("sequence_number") == 2);
            CHECK(failed->events.back().at("type") == "response.failed");
            CHECK(failed->events.back().at("response").at("error").at("code") == "server_restarted");
        }
        CHECK(store.fail_interrupted_responses() == 0U);
        const auto unchanged = store.events_after(active.id);
        CHECK(unchanged && unchanged->events.size() == 3U);
    }
}

void test_direct_node_storage_growth_is_linear() {
    temporary_directory           directory;
    const std::string             database_path = directory.database_path().string();
    sqlite_response_store         store(database_path);
    std::optional<response_state> previous;
    std::uint64_t                 bytes_at_32 = 0;

    for (std::size_t index = 0; index < 64U; ++index) {
        const std::string suffix = std::to_string(index);
        response_state    state =
            make_completed_state("resp_linear_" + suffix, "msg_linear_output_" + suffix, "msg_linear_input_" + suffix);
        state.input_items.at(0).at("content").at(0)["text"] =
            index == 0U ? "ancestor-only-sentinel" : std::string(2048, 'x');
        if (previous) {
            link_after(*previous, state);
        }
        CHECK(store.create(state) == store_write_result::stored);
        previous = std::move(state);
        if (index == 31U) {
            raw_sqlite_database raw(database_path);
            bytes_at_32 = raw.scalar_uint64("SELECT SUM(payload_bytes) FROM responses");
        }
    }

    raw_sqlite_database raw(database_path);
    const std::uint64_t bytes_at_64 = raw.scalar_uint64("SELECT SUM(payload_bytes) FROM responses");
    CHECK(bytes_at_32 > 0U);
    CHECK(bytes_at_64 > bytes_at_32);
    CHECK(bytes_at_64 < bytes_at_32 * 5U / 2U);
    const std::string last_payload = raw.scalar_text("SELECT state_json FROM responses WHERE id='resp_linear_63'");
    CHECK(last_payload.find("ancestor-only-sentinel") == std::string::npos);
    CHECK(last_payload.find("\"input\":") == std::string::npos);

    const auto materialized = store.materialize_input_items(response_id("resp_linear_63"));
    CHECK(materialized && materialized->size() == 127U);
}

void insert_legacy_v4_fixture(raw_sqlite_database & raw, const common_json & payload, const common_json & checkpoint) {
    sqlite3_stmt * response_insert = nullptr;
    const char *   response_sql =
        "INSERT INTO responses "
        "(id, revision, status, created_at, completed_at, expires_at, next_sequence_number, "
        "previous_response_id, state_json, payload_bytes) VALUES "
        "('resp_legacy_child', 1, 'completed', 1724515200, 1724515201, NULL, 0, "
        "'resp_legacy_deleted', ?, ?)";
    if (sqlite3_prepare_v2(raw.handle(), response_sql, -1, &response_insert, nullptr) != SQLITE_OK) {
        throw std::runtime_error("preparing legacy response fixture failed");
    }
    const std::string payload_text = payload.dump();
    sqlite3_bind_text(response_insert, 1, payload_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(response_insert, 2, static_cast<sqlite3_int64>(payload_text.size()));
    const int response_result = sqlite3_step(response_insert);
    sqlite3_finalize(response_insert);
    if (response_result != SQLITE_DONE) {
        throw std::runtime_error("inserting legacy response fixture failed");
    }

    sqlite3_stmt * lineage_insert = nullptr;
    if (sqlite3_prepare_v2(raw.handle(),
                           "INSERT INTO response_lineage (response_id, detached_context_json) "
                           "VALUES ('resp_legacy_child', ?)",
                           -1, &lineage_insert, nullptr) != SQLITE_OK) {
        throw std::runtime_error("preparing legacy lineage fixture failed");
    }
    const std::string checkpoint_text = checkpoint.dump();
    sqlite3_bind_text(lineage_insert, 1, checkpoint_text.c_str(), -1, SQLITE_TRANSIENT);
    const int lineage_result = sqlite3_step(lineage_insert);
    sqlite3_finalize(lineage_insert);
    if (lineage_result != SQLITE_DONE) {
        throw std::runtime_error("inserting legacy lineage fixture failed");
    }

    raw.execute(
        "INSERT INTO response_heads "
        "(response_id, revision, generation_revision, status, completed_at, next_sequence_number, journal_enabled) "
        "VALUES ('resp_legacy_child', 1, 1, 'completed', 1724515201, 0, 0);"
        "INSERT INTO response_items (item_id, response_id, output_index) "
        "VALUES ('msg_legacy_child_output', 'resp_legacy_child', 0);");
}

void test_schema_v4_legacy_checkpoint_migrates_to_tombstone_graph() {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();
    raw_sqlite_database raw(database_path);
    raw.execute(
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE responses ("
        " id TEXT PRIMARY KEY NOT NULL, revision INTEGER NOT NULL, status TEXT NOT NULL,"
        " created_at INTEGER NOT NULL, completed_at INTEGER, expires_at INTEGER,"
        " next_sequence_number INTEGER NOT NULL, previous_response_id TEXT,"
        " state_json TEXT NOT NULL, payload_bytes INTEGER NOT NULL);"
        "CREATE INDEX responses_previous_response_idx ON responses(previous_response_id);"
        "CREATE TABLE response_items ("
        " item_id TEXT PRIMARY KEY NOT NULL, response_id TEXT NOT NULL, output_index INTEGER NOT NULL,"
        " UNIQUE(response_id, output_index),"
        " FOREIGN KEY(response_id) REFERENCES responses(id) ON DELETE CASCADE);"
        "CREATE TABLE response_events ("
        " response_id TEXT NOT NULL, sequence_number INTEGER NOT NULL, event_type TEXT NOT NULL,"
        " event_json TEXT NOT NULL, payload_bytes INTEGER NOT NULL,"
        " PRIMARY KEY(response_id, sequence_number),"
        " FOREIGN KEY(response_id) REFERENCES responses(id) ON DELETE CASCADE) WITHOUT ROWID;"
        "CREATE TABLE response_heads ("
        " response_id TEXT PRIMARY KEY NOT NULL, revision INTEGER NOT NULL, generation_revision INTEGER NOT NULL,"
        " status TEXT NOT NULL, completed_at INTEGER, next_sequence_number INTEGER NOT NULL,"
        " journal_enabled INTEGER NOT NULL,"
        " FOREIGN KEY(response_id) REFERENCES responses(id) ON DELETE CASCADE) WITHOUT ROWID;"
        "CREATE TABLE response_lineage ("
        " response_id TEXT PRIMARY KEY NOT NULL, detached_context_json TEXT NOT NULL,"
        " FOREIGN KEY(response_id) REFERENCES responses(id) ON DELETE CASCADE) WITHOUT ROWID;"
        "PRAGMA user_version=4;");

    const common_json checkpoint = common_json::array({
        {
         { "id", "fc_legacy" },
         { "type", "function_call" },
         { "call_id", "call_legacy" },
         { "name", "inspect" },
         { "arguments", "{}" },
         },
        {
         { "id", "fco_legacy" },
         { "type", "function_call_output" },
         { "call_id", "call_legacy" },
         { "output", common_json::array({
                            { { "type", "input_image" }, { "image_url", "data:image/png;base64,TEVHQUNZ" } },
                            { { "type", "input_file" }, { "filename", "legacy.txt" }, { "file_data", "bGVnYWN5" } },
                        }) },
         },
    });
    const common_json direct     = common_json::array({
        {
         { "id", "msg_legacy_child_input" },
         { "type", "message" },
         { "role", "user" },
         { "content", common_json::array({ { { "type", "input_text" }, { "text", "child" } } }) },
         },
    });
    common_json       expanded   = checkpoint;
    expanded.push_back(direct.at(0));
    const common_json output_item = {
        { "id",      "msg_legacy_child_output" },
        { "call_id", nullptr                   },
        { "type",    "message"                 },
        { "value",
         {
              { "id", "msg_legacy_child_output" },
              { "type", "message" },
              { "role", "assistant" },
              { "content", common_json::array({ { { "type", "output_text" }, { "text", "legacy child" } } }) },
          }                                    },
    };
    const common_json payload = {
        { "schema_version",           1                                                                      },
        { "model",                    "fixture-model"                                                        },
        { "request",                  { { "model", "fixture-model" }, { "input", "legacy expanded input" } } },
        { "metadata",                 common_json::object()                                                  },
        { "input_items",              expanded                                                               },
        { "continuation_input_items", direct                                                                 },
        { "wire_snapshot",            { { "id", "resp_legacy_child" }, { "object", "response" } }            },
        { "detached_context",         checkpoint                                                             },
        { "output",                   common_json::array({ output_item })                                    },
        { "usage",
         {
              { "input_tokens", 1 },
              { "cached_input_tokens", 0 },
              { "output_tokens", 1 },
              { "reasoning_output_tokens", 0 },
          }                                                                                                  },
        { "error",                    nullptr                                                                },
        { "incomplete_details",       nullptr                                                                },
    };
    insert_legacy_v4_fixture(raw, payload, checkpoint);

    response_state grandchild =
        make_completed_state("resp_legacy_grandchild", "msg_legacy_grandchild_output", "msg_legacy_grandchild_input");
    {
        sqlite_response_store store(database_path);
        const auto            child = store.find(response_id("resp_legacy_child"));
        CHECK(child && child->input_items == direct);
        CHECK(child && child->legacy_lineage_checkpoint == checkpoint);
        const auto input_view = store.materialize_input_items(response_id("resp_legacy_child"));
        CHECK(input_view && input_view->size() == 3U);
        CHECK(input_view && input_view->at(0) == checkpoint.at(0));
        CHECK(input_view && input_view->at(1) == checkpoint.at(1));
        CHECK(input_view && input_view->at(2) == direct.at(0));
        CHECK(child.has_value());
        if (!child) {
            return;
        }
        link_after(*child, grandchild);
        CHECK(store.erase(child->id));
        CHECK(store.create(grandchild) == store_write_result::stored);
        CHECK(!store.find(child->id).has_value());
        CHECK(!store.find_item(item_id("msg_legacy_child_output")).has_value());
        check_item_ids(store.materialize_input_items(grandchild.id),
                       { "fc_legacy", "fco_legacy", "msg_legacy_child_input", "msg_legacy_child_output",
                         "msg_legacy_grandchild_input" });
    }

    CHECK(raw.scalar_uint64("PRAGMA user_version") == 5U);
    CHECK(raw.scalar_uint64("SELECT COUNT(*) FROM response_lineage") == 1U);
    {
        sqlite_response_store store(database_path);
        check_item_ids(store.materialize_continuation_context(grandchild.id),
                       { "fc_legacy", "fco_legacy", "msg_legacy_child_input", "msg_legacy_child_output",
                         "msg_legacy_grandchild_input", "msg_legacy_grandchild_output" });
    }
}

void test_schema_v1_migrates_to_event_journal() {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();
    sqlite3 *           database      = nullptr;
    CHECK(sqlite3_open(database_path.c_str(), &database) == SQLITE_OK);
    if (database == nullptr) {
        return;
    }
    const char * schema =
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE responses ("
        " id TEXT PRIMARY KEY NOT NULL, revision INTEGER NOT NULL, status TEXT NOT NULL,"
        " created_at INTEGER NOT NULL, completed_at INTEGER, expires_at INTEGER,"
        " next_sequence_number INTEGER NOT NULL, previous_response_id TEXT,"
        " state_json TEXT NOT NULL, payload_bytes INTEGER NOT NULL);"
        "CREATE INDEX responses_previous_response_idx ON responses(previous_response_id);"
        "CREATE TABLE response_items ("
        " item_id TEXT PRIMARY KEY NOT NULL, response_id TEXT NOT NULL, output_index INTEGER NOT NULL,"
        " UNIQUE(response_id, output_index),"
        " FOREIGN KEY(response_id) REFERENCES responses(id) ON DELETE CASCADE);"
        "PRAGMA user_version=1;";
    char * error = nullptr;
    CHECK(sqlite3_exec(database, schema, nullptr, nullptr, &error) == SQLITE_OK);
    sqlite3_free(error);

    response_state legacy = make_state("resp_legacy_v1_active", "msg_legacy_v1_unused", "msg_legacy_v1_input");
    legacy.output.clear();
    legacy.request["background"]     = true;
    legacy.request["stream"]         = true;
    legacy.next_sequence_number      = 2;
    const common_json legacy_payload = {
        { "schema_version",           1                    },
        { "model",                    legacy.model         },
        { "request",                  legacy.request       },
        { "metadata",                 legacy.metadata      },
        { "input_items",              legacy.input_items   },
        { "continuation_input_items", legacy.input_items   },
        { "wire_snapshot",            legacy.wire_snapshot },
        { "detached_context",         nullptr              },
        { "output",                   common_json::array() },
        { "usage",
         {
              { "input_tokens", 0 },
              { "cached_input_tokens", 0 },
              { "output_tokens", 0 },
              { "reasoning_output_tokens", 0 },
          }                                                },
        { "error",                    nullptr              },
        { "incomplete_details",       nullptr              },
    };
    sqlite3_stmt * insert = nullptr;
    CHECK(
        sqlite3_prepare_v2(database,
                           "INSERT INTO responses "
                           "(id, revision, status, created_at, completed_at, expires_at, next_sequence_number, "
                           "previous_response_id, state_json, payload_bytes) "
                           "VALUES ('resp_legacy_v1_active', 1, 'in_progress', 1724515200, NULL, NULL, 2, NULL, ?, ?)",
                           -1, &insert, nullptr) == SQLITE_OK);
    const std::string legacy_text = legacy_payload.dump();
    sqlite3_bind_text(insert, 1, legacy_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert, 2, static_cast<sqlite3_int64>(legacy_text.size()));
    CHECK(sqlite3_step(insert) == SQLITE_DONE);
    sqlite3_finalize(insert);
    CHECK(sqlite3_close(database) == SQLITE_OK);

    response_state        active = make_journal_state("migration");
    sqlite_response_store store(database_path);
    const auto            migrated_legacy = store.events_after(legacy.id);
    CHECK(migrated_legacy && !migrated_legacy->head.event_journal);
    CHECK(migrated_legacy && migrated_legacy->events.empty());
    CHECK(store.fail_interrupted_responses() == 1U);
    const auto recovered_legacy = store.events_after(legacy.id);
    CHECK(recovered_legacy && recovered_legacy->head.status == response_status::failed);
    CHECK(recovered_legacy && !recovered_legacy->head.event_journal);
    CHECK(recovered_legacy && recovered_legacy->events.empty());
    CHECK(store.create(active, { journal_event(0, "response.created"), journal_event(1, "response.in_progress") }) ==
          store_write_result::stored);
    const auto journal = store.events_after(active.id);
    CHECK(journal && journal->events.size() == 2U);
}

}  // namespace

int main() try {
    temporary_directory directory;
    const std::string   database_path = directory.database_path().string();

    test_persistence_and_compare_and_swap(database_path);
    test_sqlite_revision_ceiling_returns_invalid_state();
    test_restart_recovery_rejects_sqlite_integer_ceilings();
    test_immutable_graph_branches_tombstones_and_restart();
    test_multimodal_tool_lineage_survives_restart_and_parent_delete();
    test_interrupted_responses_fail_on_restart();
    test_event_journal_append_cursor_reopen_and_delete();
    test_incremental_generation_head_and_terminal_snapshot();
    test_active_child_recovers_through_tombstoned_ancestor();
    test_event_journal_active_gap_is_not_a_future_cursor();
    test_event_journal_pages_without_gaps_or_duplicates();
    test_event_journal_restart_failure_is_terminal_and_idempotent();
    test_direct_node_storage_growth_is_linear();
    test_schema_v4_legacy_checkpoint_migrates_to_tombstone_graph();
    test_schema_v1_migrates_to_event_journal();

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
