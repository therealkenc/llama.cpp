#include "sqlite-response-store.h"

#include "common.h"
#include "json.h"
#include "response-store-internal.h"
#include "response-store.h"
#include "response-types.h"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace llama_responses {
namespace {

constexpr int database_schema_version = 1;
constexpr int state_payload_version   = 1;

[[noreturn]] void throw_sqlite_error(sqlite3 * database, const std::string & operation, int result) {
    throw std::runtime_error(operation + " failed (sqlite result " + std::to_string(result) +
                             "): " + sqlite3_errmsg(database));
}

void execute(sqlite3 * database, const char * sql) {
    char * error  = nullptr;
    int    result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result == SQLITE_OK) {
        return;
    }
    const std::string message = error != nullptr ? error : sqlite3_errmsg(database);
    sqlite3_free(error);
    throw std::runtime_error("SQLite statement failed (result " + std::to_string(result) + "): " + message);
}

class sqlite_statement {
  public:
    sqlite_statement(sqlite3 * database, const char * sql) : database(database) {
        const int result = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
        if (result != SQLITE_OK) {
            throw_sqlite_error(database, "preparing SQLite statement", result);
        }
    }

    ~sqlite_statement() { sqlite3_finalize(statement); }

    sqlite_statement(const sqlite_statement &)             = delete;
    sqlite_statement & operator=(const sqlite_statement &) = delete;

    void bind_int64(int index, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
            throw std::invalid_argument("response state integer exceeds SQLite's signed 64-bit range");
        }
        const int result = sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value));
        if (result != SQLITE_OK) {
            throw_sqlite_error(database, "binding SQLite integer", result);
        }
    }

    void bind_null(int index) {
        const int result = sqlite3_bind_null(statement, index);
        if (result != SQLITE_OK) {
            throw_sqlite_error(database, "binding SQLite null", result);
        }
    }

    void bind_text(int index, const std::string & value) {
        const int result =
            sqlite3_bind_text64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        if (result != SQLITE_OK) {
            throw_sqlite_error(database, "binding SQLite text", result);
        }
    }

    bool step_row() {
        const int result = sqlite3_step(statement);
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result == SQLITE_DONE) {
            return false;
        }
        throw_sqlite_error(database, "stepping SQLite query", result);
    }

    void step_done() {
        const int result = sqlite3_step(statement);
        if (result != SQLITE_DONE) {
            throw_sqlite_error(database, "stepping SQLite statement", result);
        }
    }

    bool column_is_null(int index) const { return sqlite3_column_type(statement, index) == SQLITE_NULL; }

    std::uint64_t column_uint64(int index) const {
        const sqlite3_int64 value = sqlite3_column_int64(statement, index);
        if (value < 0) {
            throw std::runtime_error("SQLite response store contains a negative unsigned integer");
        }
        return static_cast<std::uint64_t>(value);
    }

    std::string column_text(int index) const {
        const unsigned char * value = sqlite3_column_text(statement, index);
        const int             bytes = sqlite3_column_bytes(statement, index);
        if (value == nullptr) {
            return {};
        }
        return std::string(reinterpret_cast<const char *>(value), static_cast<std::size_t>(bytes));
    }

  private:
    sqlite3 *      database  = nullptr;
    sqlite3_stmt * statement = nullptr;
};

class sqlite_transaction {
  public:
    explicit sqlite_transaction(sqlite3 * database) : database(database) { execute(database, "BEGIN IMMEDIATE"); }

    ~sqlite_transaction() {
        if (!committed) {
            sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    sqlite_transaction(const sqlite_transaction &)             = delete;
    sqlite_transaction & operator=(const sqlite_transaction &) = delete;

    void commit() {
        execute(database, "COMMIT");
        committed = true;
    }

  private:
    sqlite3 * database  = nullptr;
    bool      committed = false;
};

response_status parse_status(const std::string & value) {
    if (value == "queued") {
        return response_status::queued;
    }
    if (value == "in_progress") {
        return response_status::in_progress;
    }
    if (value == "completed") {
        return response_status::completed;
    }
    if (value == "incomplete") {
        return response_status::incomplete;
    }
    if (value == "failed") {
        return response_status::failed;
    }
    if (value == "cancelled") {
        return response_status::cancelled;
    }
    throw std::runtime_error("SQLite response store contains an unknown response status: " + value);
}

common_json serialize_payload(const response_state & state) {
    common_json output = common_json::array();
    for (const response_output_item & item : state.output) {
        common_json wire_item = {
            { "id",      item.id.str()                                                    },
            { "call_id", item.call ? common_json(item.call->str()) : common_json(nullptr) },
            { "type",    item.type                                                        },
            { "value",   item.value                                                       },
        };
        output.push_back(std::move(wire_item));
    }

    common_json error = nullptr;
    if (state.error) {
        error = {
            { "code",    state.error->code    },
            { "message", state.error->message },
            { "param",   state.error->param   },
        };
    }

    return {
        { "schema_version",           state_payload_version                                                   },
        { "model",                    state.model                                                             },
        { "request",                  state.request                                                           },
        { "metadata",                 state.metadata                                                          },
        { "input_items",              state.input_items                                                       },
        { "continuation_input_items", state.continuation_input_items                                          },
        { "wire_snapshot",            state.wire_snapshot                                                     },
        { "detached_context",         state.detached_context ? *state.detached_context : common_json(nullptr) },
        { "output",                   std::move(output)                                                       },
        { "usage",
         {
              { "input_tokens", state.usage.input_tokens },
              { "cached_input_tokens", state.usage.cached_input_tokens },
              { "output_tokens", state.usage.output_tokens },
              { "reasoning_output_tokens", state.usage.reasoning_output_tokens },
          }                                                                                                   },
        { "error",                    std::move(error)                                                        },
        { "incomplete_details",       state.incomplete_details                                                },
    };
}

response_state deserialize_payload(const std::string & payload_text) {
    common_json payload = common_json::parse(payload_text);
    if (!payload.is_object() || payload.value("schema_version", 0) != state_payload_version) {
        throw std::runtime_error("SQLite response store contains an unsupported state payload version");
    }

    response_state state;
    state.model       = payload.at("model").get<std::string>();
    state.request     = payload.at("request");
    state.metadata    = payload.at("metadata");
    state.input_items = payload.at("input_items");
    state.continuation_input_items =
        payload.contains("continuation_input_items") ? payload.at("continuation_input_items") : state.input_items;
    state.wire_snapshot      = payload.at("wire_snapshot");
    state.incomplete_details = payload.at("incomplete_details");

    if (payload.contains("detached_context") && !payload.at("detached_context").is_null()) {
        state.detached_context = payload.at("detached_context");
    }

    for (const common_json & wire_item : payload.at("output")) {
        response_output_item item;
        item.id    = item_id(wire_item.at("id").get<std::string>());
        item.type  = wire_item.at("type").get<std::string>();
        item.value = wire_item.at("value");
        if (wire_item.contains("call_id") && !wire_item.at("call_id").is_null()) {
            item.call = call_id(wire_item.at("call_id").get<std::string>());
        }
        state.output.push_back(std::move(item));
    }

    const common_json & usage           = payload.at("usage");
    state.usage.input_tokens            = usage.at("input_tokens").get<std::uint64_t>();
    state.usage.cached_input_tokens     = usage.at("cached_input_tokens").get<std::uint64_t>();
    state.usage.output_tokens           = usage.at("output_tokens").get<std::uint64_t>();
    state.usage.reasoning_output_tokens = usage.at("reasoning_output_tokens").get<std::uint64_t>();

    if (payload.contains("error") && !payload.at("error").is_null()) {
        response_error error;
        error.code    = payload.at("error").at("code").get<std::string>();
        error.message = payload.at("error").at("message").get<std::string>();
        error.param   = payload.at("error").at("param").get<std::string>();
        state.error   = std::move(error);
    }
    return state;
}

}  // namespace

class sqlite_response_store::impl {
  public:
    explicit impl(const std::string & path) {
        if (path.empty()) {
            throw std::invalid_argument("SQLite response store path must not be empty");
        }
        const int result = sqlite3_open_v2(path.c_str(), &database,
                                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (result != SQLITE_OK) {
            const std::string message = database != nullptr ? sqlite3_errmsg(database) : "unable to allocate handle";
            if (database != nullptr) {
                sqlite3_close_v2(database);
                database = nullptr;
            }
            throw std::runtime_error("opening SQLite response store failed: " + message);
        }

        try {
            const int timeout_result = sqlite3_busy_timeout(database, 5000);
            if (timeout_result != SQLITE_OK) {
                throw_sqlite_error(database, "configuring SQLite busy timeout", timeout_result);
            }
            execute(database, "PRAGMA foreign_keys=ON");
            execute(database, "PRAGMA journal_mode=WAL");
            execute(database, "PRAGMA synchronous=NORMAL");
            migrate();
        } catch (...) {
            sqlite3_close_v2(database);
            database = nullptr;
            throw;
        }
    }

    ~impl() {
        if (database != nullptr) {
            sqlite3_close_v2(database);
        }
    }

    store_write_result create(response_state state) {
        if (!response_store_detail::valid_state(state) || !integers_fit(state)) {
            return store_write_result::invalid_state;
        }

        std::lock_guard<std::mutex> lock(mutex);
        sqlite_transaction          transaction(database);
        if (load(state.id)) {
            return store_write_result::already_exists;
        }
        if (!item_ids_available(state)) {
            return store_write_result::item_id_conflict;
        }

        state.revision            = 1;
        const std::string payload = serialize_payload(state).dump();
        sqlite_statement  insert(database,
                                 "INSERT INTO responses "
                                 "(id, revision, status, created_at, completed_at, expires_at, "
                                 " next_sequence_number, previous_response_id, state_json, payload_bytes) "
                                 "VALUES (?, ?, ?, ?, ?, NULL, ?, ?, ?, ?)");
        bind_state_columns(insert, state, payload, 1);
        insert.step_done();
        insert_item_index(state);
        transaction.commit();
        return store_write_result::stored;
    }

    store_write_result replace(response_state state) {
        if (!response_store_detail::valid_state(state) || !integers_fit(state)) {
            return store_write_result::invalid_state;
        }

        std::lock_guard<std::mutex> lock(mutex);
        sqlite_transaction          transaction(database);
        const auto                  current = load(state.id);
        if (!current) {
            return store_write_result::not_found;
        }
        if (state.revision != current->revision) {
            return store_write_result::stale_revision;
        }
        if (!response_status_can_transition(current->status, state.status)) {
            return store_write_result::invalid_transition;
        }
        if (!item_ids_available(state)) {
            return store_write_result::item_id_conflict;
        }
        if (!state.detached_context && current->detached_context) {
            state.detached_context = current->detached_context;
        }
        if (state.revision == std::numeric_limits<std::uint64_t>::max()) {
            return store_write_result::invalid_state;
        }

        const std::uint64_t expected_revision = state.revision;
        state.revision++;
        const std::string payload = serialize_payload(state).dump();

        sqlite_statement update(database,
                                "UPDATE responses SET revision=?, status=?, created_at=?, completed_at=?, "
                                "next_sequence_number=?, previous_response_id=?, state_json=?, payload_bytes=? "
                                "WHERE id=? AND revision=?");
        update.bind_int64(1, state.revision);
        update.bind_text(2, response_status_name(state.status));
        update.bind_int64(3, state.created_at);
        bind_optional_uint64(update, 4, state.completed_at);
        update.bind_int64(5, state.next_sequence_number);
        bind_optional_id(update, 6, state.previous_response);
        update.bind_text(7, payload);
        update.bind_int64(8, payload.size());
        update.bind_text(9, state.id.str());
        update.bind_int64(10, expected_revision);
        update.step_done();
        if (sqlite3_changes(database) != 1) {
            return store_write_result::stale_revision;
        }

        sqlite_statement remove_items(database, "DELETE FROM response_items WHERE response_id=?");
        remove_items.bind_text(1, state.id.str());
        remove_items.step_done();
        insert_item_index(state);
        transaction.commit();
        return store_write_result::stored;
    }

    std::optional<response_state> find(const response_id & id) const {
        std::lock_guard<std::mutex> lock(mutex);
        return load(id);
    }

    std::optional<stored_response_item> find_item(const item_id & id) const {
        std::lock_guard<std::mutex> lock(mutex);
        sqlite_statement statement(database, "SELECT response_id, output_index FROM response_items WHERE item_id=?");
        statement.bind_text(1, id.str());
        if (!statement.step_row()) {
            return std::nullopt;
        }
        const response_id   owner(statement.column_text(0));
        const std::uint64_t output_index = statement.column_uint64(1);
        const auto          state        = load(owner);
        if (!state || output_index >= state->output.size() || state->output[output_index].id != id) {
            throw std::runtime_error("SQLite response item index is inconsistent with its response payload");
        }
        return stored_response_item{ owner, state->output[output_index] };
    }

    bool erase(const response_id & id) {
        std::lock_guard<std::mutex> lock(mutex);
        sqlite_transaction          transaction(database);
        const auto                  state = load(id);
        if (!state) {
            return false;
        }

        const common_json        context = response_store_detail::detached_context(*state);
        std::vector<response_id> children;
        sqlite_statement select_children(database, "SELECT id FROM responses WHERE previous_response_id=? ORDER BY id");
        select_children.bind_text(1, id.str());
        while (select_children.step_row()) {
            children.emplace_back(select_children.column_text(0));
        }

        for (const response_id & child_id : children) {
            auto child = load(child_id);
            if (!child || child->revision == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("unable to detach SQLite response descendant");
            }
            const std::uint64_t expected_revision = child->revision;
            child->detached_context               = context;
            child->revision++;
            const std::string payload = serialize_payload(*child).dump();

            sqlite_statement update_child(
                database, "UPDATE responses SET revision=?, state_json=?, payload_bytes=? WHERE id=? AND revision=?");
            update_child.bind_int64(1, child->revision);
            update_child.bind_text(2, payload);
            update_child.bind_int64(3, payload.size());
            update_child.bind_text(4, child_id.str());
            update_child.bind_int64(5, expected_revision);
            update_child.step_done();
            if (sqlite3_changes(database) != 1) {
                throw std::runtime_error("SQLite response descendant changed while detaching its context");
            }
        }

        sqlite_statement remove(database, "DELETE FROM responses WHERE id=?");
        remove.bind_text(1, id.str());
        remove.step_done();
        if (sqlite3_changes(database) != 1) {
            return false;
        }
        transaction.commit();
        return true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        sqlite_statement            statement(database, "SELECT COUNT(*) FROM responses");
        if (!statement.step_row()) {
            throw std::runtime_error("SQLite response count query returned no row");
        }
        return static_cast<std::size_t>(statement.column_uint64(0));
    }

    std::size_t fail_interrupted_responses() {
        std::lock_guard<std::mutex> lock(mutex);
        sqlite_transaction          transaction(database);
        std::vector<response_id>    active_ids;
        sqlite_statement active_query(database,
                                      "SELECT id FROM responses WHERE status IN ('queued', 'in_progress') ORDER BY id");
        while (active_query.step_row()) {
            active_ids.emplace_back(active_query.column_text(0));
        }

        std::size_t failed_count = 0;
        for (const response_id & id : active_ids) {
            auto state = load(id);
            if (!state || response_status_is_terminal(state->status)) {
                continue;
            }
            if (state->revision == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("unable to terminalize an interrupted response at maximum revision");
            }

            const std::uint64_t expected_revision = state->revision;
            state->revision++;
            state->status       = response_status::failed;
            state->completed_at = std::nullopt;
            state->error        = response_error{
                "server_restarted",
                "Response execution was interrupted because llama-server restarted.",
                "",
            };
            state->incomplete_details = nullptr;
            const std::string payload = serialize_payload(*state).dump();

            sqlite_statement update(database,
                                    "UPDATE responses SET revision=?, status=?, completed_at=NULL, "
                                    "state_json=?, payload_bytes=? WHERE id=? AND revision=?");
            update.bind_int64(1, state->revision);
            update.bind_text(2, response_status_name(state->status));
            update.bind_text(3, payload);
            update.bind_int64(4, payload.size());
            update.bind_text(5, state->id.str());
            update.bind_int64(6, expected_revision);
            update.step_done();
            if (sqlite3_changes(database) != 1) {
                throw std::runtime_error("interrupted response changed while terminalizing it");
            }
            failed_count++;
        }
        transaction.commit();
        return failed_count;
    }

  private:
    std::uint64_t schema_version() const {
        sqlite_statement version_query(database, "PRAGMA user_version");
        if (!version_query.step_row()) {
            throw std::runtime_error("SQLite response store did not report a schema version");
        }
        return version_query.column_uint64(0);
    }

    void migrate() {
        std::uint64_t version = schema_version();
        if (version > database_schema_version) {
            throw std::runtime_error("SQLite response store schema is newer than this llama-server supports");
        }
        if (version == database_schema_version) {
            return;
        }
        if (version != 0) {
            throw std::runtime_error("SQLite response store requires an unavailable schema migration");
        }

        sqlite_transaction transaction(database);
        // Another llama-server may have initialized a shared cache database
        // while this connection waited for the write lock.
        version = schema_version();
        if (version == database_schema_version) {
            transaction.commit();
            return;
        }
        if (version != 0) {
            throw std::runtime_error("SQLite response store schema changed while acquiring the migration lock");
        }
        execute(database,
                "CREATE TABLE responses ("
                " id TEXT PRIMARY KEY NOT NULL,"
                " revision INTEGER NOT NULL,"
                " status TEXT NOT NULL,"
                " created_at INTEGER NOT NULL,"
                " completed_at INTEGER,"
                " expires_at INTEGER,"
                " next_sequence_number INTEGER NOT NULL,"
                " previous_response_id TEXT,"
                " state_json TEXT NOT NULL,"
                " payload_bytes INTEGER NOT NULL"
                ")");
        execute(database, "CREATE INDEX responses_previous_response_idx ON responses(previous_response_id)");
        execute(database,
                "CREATE TABLE response_items ("
                " item_id TEXT PRIMARY KEY NOT NULL,"
                " response_id TEXT NOT NULL,"
                " output_index INTEGER NOT NULL,"
                " UNIQUE(response_id, output_index),"
                " FOREIGN KEY(response_id) REFERENCES responses(id) ON DELETE CASCADE"
                ")");
        execute(database, "PRAGMA user_version=1");
        transaction.commit();
    }

    static bool integers_fit(const response_state & state) {
        constexpr std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max());
        return state.created_at <= maximum && state.revision <= maximum && state.next_sequence_number <= maximum &&
               (!state.completed_at || *state.completed_at <= maximum) && state.output.size() <= maximum;
    }

    static void bind_optional_uint64(sqlite_statement &                   statement,
                                     int                                  index,
                                     const std::optional<std::uint64_t> & value) {
        if (value) {
            statement.bind_int64(index, *value);
        } else {
            statement.bind_null(index);
        }
    }

    static void bind_optional_id(sqlite_statement & statement, int index, const std::optional<response_id> & value) {
        if (value) {
            statement.bind_text(index, value->str());
        } else {
            statement.bind_null(index);
        }
    }

    static void bind_state_columns(sqlite_statement &     statement,
                                   const response_state & state,
                                   const std::string &    payload,
                                   int                    first_index) {
        statement.bind_text(first_index, state.id.str());
        statement.bind_int64(first_index + 1, state.revision);
        statement.bind_text(first_index + 2, response_status_name(state.status));
        statement.bind_int64(first_index + 3, state.created_at);
        bind_optional_uint64(statement, first_index + 4, state.completed_at);
        statement.bind_int64(first_index + 5, state.next_sequence_number);
        bind_optional_id(statement, first_index + 6, state.previous_response);
        statement.bind_text(first_index + 7, payload);
        statement.bind_int64(first_index + 8, payload.size());
    }

    std::optional<response_state> load(const response_id & id) const {
        sqlite_statement statement(database,
                                   "SELECT id, revision, status, created_at, completed_at, "
                                   "next_sequence_number, previous_response_id, state_json "
                                   "FROM responses WHERE id=?");
        statement.bind_text(1, id.str());
        if (!statement.step_row()) {
            return std::nullopt;
        }

        response_state state = deserialize_payload(statement.column_text(7));
        state.id             = response_id(statement.column_text(0));
        state.revision       = statement.column_uint64(1);
        state.status         = parse_status(statement.column_text(2));
        state.created_at     = statement.column_uint64(3);
        if (!statement.column_is_null(4)) {
            state.completed_at = statement.column_uint64(4);
        }
        state.next_sequence_number = statement.column_uint64(5);
        if (!statement.column_is_null(6)) {
            state.previous_response = response_id(statement.column_text(6));
        }
        if (!response_store_detail::valid_state(state)) {
            throw std::runtime_error("SQLite response store contains an invalid response state");
        }
        return state;
    }

    bool item_ids_available(const response_state & state) const {
        for (const response_output_item & item : state.output) {
            sqlite_statement statement(database, "SELECT response_id FROM response_items WHERE item_id=?");
            statement.bind_text(1, item.id.str());
            if (statement.step_row() && statement.column_text(0) != state.id.str()) {
                return false;
            }
        }
        return true;
    }

    void insert_item_index(const response_state & state) {
        for (std::size_t index = 0; index < state.output.size(); ++index) {
            sqlite_statement statement(
                database, "INSERT INTO response_items (item_id, response_id, output_index) VALUES (?, ?, ?)");
            statement.bind_text(1, state.output[index].id.str());
            statement.bind_text(2, state.id.str());
            statement.bind_int64(3, index);
            statement.step_done();
        }
    }

    sqlite3 *          database = nullptr;
    mutable std::mutex mutex;
};

std::string default_sqlite_response_store_path() {
    std::string override_path = common_get_env("LLAMA_RESPONSES_DB");
    if (!override_path.empty()) {
        return override_path;
    }
    return fs_get_cache_file("responses.sqlite3");
}

sqlite_response_store::sqlite_response_store(const std::string & path) : pimpl(std::make_unique<impl>(path)) {}

sqlite_response_store::~sqlite_response_store() = default;

store_write_result sqlite_response_store::create(response_state state) {
    return pimpl->create(std::move(state));
}

store_write_result sqlite_response_store::replace(response_state state) {
    return pimpl->replace(std::move(state));
}

std::optional<response_state> sqlite_response_store::find(const response_id & id) const {
    return pimpl->find(id);
}

std::optional<stored_response_item> sqlite_response_store::find_item(const item_id & id) const {
    return pimpl->find_item(id);
}

bool sqlite_response_store::erase(const response_id & id) {
    return pimpl->erase(id);
}

std::size_t sqlite_response_store::size() const {
    return pimpl->size();
}

std::size_t sqlite_response_store::fail_interrupted_responses() {
    return pimpl->fail_interrupted_responses();
}

}  // namespace llama_responses
