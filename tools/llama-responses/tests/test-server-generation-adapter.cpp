#include "chat.h"
#include "generation.h"
#include "json.h"
#include "protocol-codec.h"
#include "response-store.h"
#include "response-types.h"
#include "server-generation-adapter.h"
#include "server-generation.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

generation_response_context fixture_context(const std::string & suffix) {
    generation_response_context context;
    context.model      = "fixture-model";
    context.created_at = 100;
    context.request    = {
        { "model", context.model },
        { "input", "fixture"     },
        { "store", true          },
    };
    context.input_items = common_json::array({
        {
         { "id", "msg_input_" + suffix },
         { "type", "message" },
         { "role", "user" },
         { "content", common_json::array({
                             {
                                 { "type", "input_text" },
                                 { "text", "fixture" },
                             },
                         }) },
         },
    });
    return context;
}

std::vector<common_json> parse_sse(const std::string & payload) {
    std::vector<common_json> events;
    std::size_t              position = 0;
    while ((position = payload.find("data: ", position)) != std::string::npos) {
        position += 6;
        const std::size_t line_end = payload.find('\n', position);
        events.push_back(common_json::parse(payload.substr(position, line_end - position)));
        position = line_end == std::string::npos ? payload.size() : line_end + 1;
    }
    return events;
}

std::vector<common_json> events_named(const std::vector<common_json> & events, const std::string & type) {
    std::vector<common_json> result;
    for (const common_json & event : events) {
        if (event.value("type", std::string()) == type) {
            result.push_back(event);
        }
    }
    return result;
}

server_generation_usage fixture_usage() {
    return {
        23,
        5,
        9,
    };
}

void test_text_reasoning_reconciliation_and_persistence() {
    in_memory_response_store      store;
    native_server_generation_sink sink(fixture_context("text"), "adapter_text", true, &store);

    std::string          stream = sink.accept(server_generation_started{});
    common_chat_msg_diff reasoning;
    reasoning.reasoning_content_delta = "inspect";
    common_chat_msg_diff text;
    text.content_delta = "answer<STOP";
    stream += sink.accept(server_generation_message_deltas{
        { reasoning, text }
    });

    common_chat_msg final_message;
    final_message.reasoning_content = "inspect";
    final_message.content           = "answer";
    stream += sink.accept(server_generation_message_snapshot{ final_message });
    stream += sink.accept(server_generation_completed{ fixture_usage(), 101 });

    CHECK(sink.terminal());
    const common_json snapshot = sink.snapshot();
    CHECK(snapshot.at("status") == "completed");
    CHECK(snapshot.at("output_text") == "answer");
    CHECK(snapshot.at("usage").at("input_tokens") == 23);

    const auto stored = store.find(sink.id());
    CHECK(stored.has_value());
    if (stored) {
        CHECK(stored->status == response_status::completed);
        // Initial create plus one terminal snapshot. Intermediate output lives
        // in the active projection instead of rewriting the resource.
        CHECK(stored->revision == 2);
        CHECK(render_response(*stored).at("output_text") == "answer");
    }

    const std::vector<common_json> events = parse_sse(stream);
    CHECK(!events.empty());
    for (std::size_t index = 0; index < events.size(); ++index) {
        CHECK(events[index].at("sequence_number").get<std::size_t>() == index);
    }
    CHECK(events_named(events, "response.reasoning_text.delta").size() == 1);
    CHECK(events_named(events, "response.reasoning_text.done").size() == 1);
    CHECK(events_named(events, "response.output_text.delta").at(0).at("delta") == "answer<STOP");
    CHECK(events.back().at("type") == "response.completed");
    CHECK(events.back().at("response").at("output_text") == "answer");
}

common_chat_msg_diff tool_diff(std::size_t         index,
                               const std::string & name,
                               const std::string & id,
                               const std::string & arguments) {
    common_chat_msg_diff result;
    result.tool_call_index           = index;
    result.tool_call_delta.name      = name;
    result.tool_call_delta.id        = id;
    result.tool_call_delta.arguments = arguments;
    return result;
}

void test_interleaved_namespace_custom_and_local_shell_tools() {
    const std::unordered_map<std::string, common_json> metadata = {
        {
         "flat_patch",  {
                { "type", "custom" },
                { "name", "apply_patch" },
                { "namespace", "mcp__editing" },
            }, },
        {
         "flat_lookup",            {
                { "type", "function" },
                { "name", "lookup" },
                { "namespace", "mcp__calendar" },
            }, },
        {
         "local_shell", {
                { "type", "local_shell" },
                { "name", "local_shell" },
            }, },
    };
    native_server_generation_sink sink(fixture_context("tools"), "adapter_tools", true, nullptr, metadata);

    std::string stream = sink.accept(server_generation_started{});
    stream += sink.accept(server_generation_message_deltas{
        {
         tool_diff(1, "flat_patch", "patch_id", R"({"input":"*** Begin Patch\n@@ -1,1 +1,1 @@ context\n)"),
         tool_diff(0, "flat_lookup", "lookup_id", R"({"date":)"),
         },
    });
    common_chat_msg_diff patch_tail  = tool_diff(1, "", "", R"(*** End Patch"})");
    common_chat_msg_diff lookup_tail = tool_diff(0, "", "", R"("today"})");
    common_chat_msg_diff shell = tool_diff(2, "local_shell", "shell_id", R"({"command":"pwd","timeout_ms":1000})");
    stream += sink.accept(server_generation_message_deltas{
        { lookup_tail, patch_tail, shell }
    });

    common_chat_msg final_message;
    final_message.tool_calls = {
        { "flat_lookup", R"({"date":"today"})",                                                    "lookup_id" },
        { "flat_patch",  R"({"input":"*** Begin Patch\n@@ -1,1 +1,1 @@ context\n*** End Patch"})", "patch_id"  },
        { "local_shell", R"({"command":"pwd","timeout_ms":1000})",                                 "shell_id"  },
    };
    stream += sink.accept(server_generation_message_snapshot{ final_message });
    stream += sink.accept(server_generation_completed{ fixture_usage(), 101 });

    const common_json snapshot = sink.snapshot();
    CHECK(snapshot.at("output").size() == 3);
    const common_json & custom     = snapshot.at("output").at(0);
    const common_json & function   = snapshot.at("output").at(1);
    const common_json & shell_item = snapshot.at("output").at(2);
    CHECK(custom.at("type") == "custom_tool_call");
    CHECK(custom.at("name") == "apply_patch");
    CHECK(custom.at("namespace") == "mcp__editing");
    CHECK(custom.at("input") == "*** Begin Patch\n@@ context\n*** End Patch");
    CHECK(custom.at("id").get<std::string>().rfind("ctc_", 0) == 0);
    CHECK(custom.at("call_id") == "call_patch_id");
    CHECK(function.at("type") == "function_call");
    CHECK(function.at("name") == "lookup");
    CHECK(function.at("namespace") == "mcp__calendar");
    CHECK(function.at("arguments") == R"({"date":"today"})");
    CHECK(shell_item.at("type") == "local_shell_call");
    CHECK(shell_item.at("action").at("type") == "exec");
    CHECK(shell_item.at("action").at("command") == common_json::array({ "bash", "-lc", "pwd" }));
    CHECK(shell_item.at("action").at("timeout_ms") == 1000);

    const std::vector<common_json> events = parse_sse(stream);
    CHECK(events_named(events, "response.custom_tool_call_input.delta").size() == 1);
    CHECK(events_named(events, "response.function_call_arguments.delta").size() == 2);
    CHECK(events.back().at("response") == snapshot);
}

void test_partial_tool_name_is_buffered_until_arguments() {
    const std::unordered_map<std::string, common_json> metadata = {
        {
         "web_search", {
                { "type", "function" },
                { "name", "web_search" },
            }, },
    };
    native_server_generation_sink sink(fixture_context("partial_name"), "adapter_partial_name", true, nullptr,
                                       metadata);

    std::string stream = sink.accept(server_generation_started{});
    stream += sink.accept(server_generation_message_deltas{ { tool_diff(0, "web", "tool_id", "") } });
    stream += sink.accept(server_generation_message_deltas{ { tool_diff(0, "web_search", "", "") } });
    stream += sink.accept(server_generation_message_deltas{ { tool_diff(0, "", "", "{}") } });

    common_chat_msg final_message;
    final_message.tool_calls = {
        { "web_search", "{}", "tool_id" }
    };
    stream += sink.accept(server_generation_message_snapshot{ final_message });
    stream += sink.accept(server_generation_completed{ fixture_usage(), 101 });

    const common_json snapshot = sink.snapshot();
    CHECK(snapshot.at("output").size() == 1);
    CHECK(snapshot.at("output").at(0).at("name") == "web_search");
    CHECK(events_named(parse_sse(stream), "response.output_item.added").size() == 1);
}

void test_custom_argument_boundary_ignores_nested_string_syntax() {
    const std::unordered_map<std::string, common_json> metadata = {
        {
         "flat_custom", {
                { "type", "custom" },
                { "name", "code" },
            }, },
    };
    native_server_generation_sink sink(fixture_context("custom_boundary"), "adapter_custom_boundary", true, nullptr,
                                       metadata);

    const std::string first  = R"({"input":"brace { and )";
    const std::string second = R"(quote \" still text)";
    const std::string third  = R"("})";
    const std::string raw    = first + second + third;

    std::string stream = sink.accept(server_generation_started{});
    stream += sink.accept(server_generation_message_deltas{ { tool_diff(0, "flat_custom", "custom_id", first) } });
    stream += sink.accept(server_generation_message_deltas{ { tool_diff(0, "", "", second) } });
    CHECK(events_named(parse_sse(stream), "response.custom_tool_call_input.delta").empty());
    stream += sink.accept(server_generation_message_deltas{ { tool_diff(0, "", "", third) } });

    common_chat_msg final_message;
    final_message.tool_calls = {
        { "flat_custom", raw, "custom_id" }
    };
    stream += sink.accept(server_generation_message_snapshot{ final_message });
    stream += sink.accept(server_generation_completed{ fixture_usage(), 101 });

    const common_json snapshot = sink.snapshot();
    CHECK(snapshot.at("output").at(0).at("input") == "brace { and quote \" still text");
    CHECK(events_named(parse_sse(stream), "response.custom_tool_call_input.delta").size() == 1U);
}

class failing_store final : public response_store {
  public:
    store_write_result create(response_state /*state*/, const std::vector<common_json> & /*events*/) override {
        return store_write_result::invalid_state;
    }

    store_write_result replace(response_state /*state*/, const std::vector<common_json> & /*events*/) override {
        return store_write_result::invalid_state;
    }

    generation_store_write advance_generation(const response_state & /*state*/,
                                              std::uint64_t /*expected_generation_revision*/,
                                              const std::vector<common_json> & /*events*/) override {
        return { store_write_result::invalid_state, 0 };
    }

    std::optional<response_state> find(const response_id & /*id*/) const override { return std::nullopt; }

    std::optional<stored_response_item> find_item(const item_id & /*id*/) const override { return std::nullopt; }

    std::optional<common_json> materialize_input_items(const response_id & /*id*/) const override {
        return std::nullopt;
    }

    std::optional<common_json> materialize_continuation_context(const response_id & /*id*/) const override {
        return std::nullopt;
    }

    std::optional<response_event_page> events_after(
        const response_id & /*id*/,
        const std::optional<std::uint64_t> & /*starting_after*/) const override {
        return std::nullopt;
    }

    bool wait_for_event_change(std::uint64_t /*observed_epoch*/, std::uint64_t /*timeout_ms*/) const override {
        return false;
    }

    bool erase(const response_id & /*id*/) override { return false; }

    std::size_t size() const override { return 0; }
};

void test_checkpoint_failure_and_cancellation_contract() {
    failing_store                 store;
    native_server_generation_sink sink(fixture_context("failure"), "adapter_failure", false, &store);
    bool                          threw = false;
    try {
        sink.accept(server_generation_started{});
    } catch (const std::runtime_error & error) {
        threw = std::string(error.what()).find("could not be persisted") != std::string::npos;
    }
    CHECK(threw);
    CHECK(sink.storage_failed());
    CHECK(sink.cancel_requested());

    native_server_generation_sink cancellable(fixture_context("cancel"), "adapter_cancel", true);
    CHECK(!cancellable.cancel_requested());
    cancellable.request_cancel();
    CHECK(cancellable.cancel_requested());
}

void test_tombstoned_parent_does_not_revise_active_child() {
    in_memory_response_store store;

    native_server_generation_sink parent(fixture_context("detach_parent"), "adapter_detach_parent", false, &store);
    parent.accept(server_generation_started{});
    common_chat_msg parent_message;
    parent_message.content = "parent output";
    parent.accept(server_generation_message_snapshot{ std::move(parent_message) });
    parent.accept(server_generation_completed{ fixture_usage(), 101 });

    generation_response_context child_context = fixture_context("detach_child");
    child_context.previous_response           = parent.id();
    child_context.request["background"]       = true;
    child_context.request["stream"]           = true;
    native_server_generation_sink child(std::move(child_context), "adapter_detach_child", true, &store, {}, true);
    child.accept(server_generation_started{});

    const auto initial_child = store.find(child.id());
    CHECK(initial_child && initial_child->revision == 1U);
    CHECK(store.erase(parent.id()));
    CHECK(!store.find(parent.id()).has_value());
    const auto unchanged_child = store.find(child.id());
    CHECK(unchanged_child && unchanged_child->revision == 1U);
    CHECK(unchanged_child && !unchanged_child->legacy_lineage_checkpoint.has_value());

    const auto input_items = store.materialize_input_items(child.id());
    CHECK(input_items && input_items->size() == 3U);
    if (input_items && input_items->size() == 3U) {
        CHECK(input_items->at(0).at("id") == "msg_input_detach_parent");
        CHECK(input_items->at(1).at("role") == "assistant");
        CHECK(input_items->at(2).at("id") == "msg_input_detach_child");
    }

    common_chat_msg_diff text;
    text.content_delta       = "still streaming";
    const std::string stream = child.accept(server_generation_message_deltas{ { text } });
    CHECK(stream.find("response_store_error") == std::string::npos);
    CHECK(!child.storage_failed());

    const auto updated = store.find(child.id());
    CHECK(updated.has_value());
    CHECK(updated && updated->revision == 2U);
    CHECK(updated && !updated->legacy_lineage_checkpoint.has_value());
    const auto journal = store.events_after(child.id());
    CHECK(journal && journal->events.size() == 5U);
    if (journal) {
        for (std::size_t index = 0; index < journal->events.size(); ++index) {
            CHECK(journal->events[index].at("sequence_number") == index);
        }
    }
}

void test_journaled_incomplete_and_failed_terminals() {
    in_memory_response_store store;

    generation_response_context incomplete_context = fixture_context("journal_incomplete");
    incomplete_context.request["background"]       = true;
    incomplete_context.request["stream"]           = true;
    native_server_generation_sink incomplete(std::move(incomplete_context), "adapter_journal_incomplete", true, &store,
                                             {}, true);
    incomplete.accept(server_generation_started{});
    incomplete.accept(server_generation_incomplete{ fixture_usage(), "max_output_tokens" });
    const auto incomplete_journal = store.events_after(incomplete.id());
    CHECK(incomplete_journal && incomplete_journal->head.status == response_status::incomplete);
    CHECK(incomplete_journal && !incomplete_journal->events.empty());
    if (incomplete_journal && !incomplete_journal->events.empty()) {
        CHECK(incomplete_journal->events.back().at("type") == "response.incomplete");
    }

    generation_response_context failed_context = fixture_context("journal_failed");
    failed_context.request["background"]       = true;
    failed_context.request["stream"]           = true;
    native_server_generation_sink failed(std::move(failed_context), "adapter_journal_failed", true, &store, {}, true);
    failed.accept(server_generation_started{});
    failed.accept(server_generation_failed{
        { "server_error", "fixture failure", "" },
        std::nullopt,
    });
    const auto failed_journal = store.events_after(failed.id());
    CHECK(failed_journal && failed_journal->head.status == response_status::failed);
    CHECK(failed_journal && !failed_journal->events.empty());
    if (failed_journal && !failed_journal->events.empty()) {
        CHECK(failed_journal->events.back().at("type") == "response.failed");
    }
}

void test_translated_update_failure_preserves_a_contiguous_terminal() {
    in_memory_response_store store;

    generation_response_context context = fixture_context("atomic_update");
    context.request["background"]       = true;
    context.request["stream"]           = true;
    native_server_generation_sink sink(std::move(context), "adapter_atomic_update", true, &store, {}, true);
    sink.accept(server_generation_started{});

    common_chat_msg_diff text;
    text.content_delta = "preserved before failure";
    common_chat_msg_diff invalid_reasoning;
    invalid_reasoning.reasoning_content_delta = "reasoning cannot follow text";
    bool threw                                = false;
    try {
        sink.accept(server_generation_message_deltas{
            { text, invalid_reasoning }
        });
    } catch (const std::logic_error &) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!sink.storage_failed());

    const auto active       = store.events_after(sink.id());
    const auto active_state = store.find(sink.id());
    // The durable checkpoint remains at the last complete server update until
    // the caller translates the projection error into a failed terminal.
    CHECK(active && active->head.next_sequence_number == 2U);
    CHECK(active_state && active_state->output.empty());
    CHECK(active && active->events.size() == 2U);

    const std::string terminal = sink.accept(server_generation_failed{
        { "server_error", "invalid generated delta order", "" },
        std::nullopt,
    });
    CHECK(terminal.find("preserved before failure") != std::string::npos);
    CHECK(terminal.find("response.output_text.delta") != std::string::npos);
    CHECK(terminal.find("response.failed") != std::string::npos);
    CHECK(terminal.find("response_store_error") == std::string::npos);
    const auto failed = store.events_after(sink.id());
    CHECK(failed && failed->head.status == response_status::failed);
    CHECK(failed && failed->events.size() == failed->head.next_sequence_number);
    if (failed) {
        for (std::size_t index = 0; index < failed->events.size(); ++index) {
            CHECK(failed->events[index].at("sequence_number") == index);
        }
        CHECK(failed->events.back().at("type") == "response.failed");
    }
}

void test_translation_failure_poisoning_terminalizes_from_the_last_checkpoint() {
    in_memory_response_store    store;
    generation_response_context context                         = fixture_context("translation_failure");
    context.request["background"]                               = true;
    context.request["stream"]                                   = true;
    const std::unordered_map<std::string, common_json> metadata = {
        {
         "unsupported_tool", {
                { "type", "not_a_generation_tool" },
            }, },
    };
    native_server_generation_sink sink(std::move(context), "adapter_translation_failure", true, &store, metadata, true);
    sink.accept(server_generation_started{});

    common_chat_msg_diff text;
    text.content_delta               = "buffered by the rejected translation";
    const common_chat_msg_diff tool  = tool_diff(0, "unsupported_tool", "bad_tool", "{}");
    bool                       threw = false;
    try {
        sink.accept(server_generation_message_deltas{
            { text, tool }
        });
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        sink.accept(server_generation_progress{});
    } catch (const std::logic_error &) {
        threw = true;
    }
    CHECK(threw);

    const std::string terminal = sink.accept(server_generation_failed{
        { "server_error", "invalid generated tool type", "" },
        std::nullopt,
    });
    CHECK(terminal.find("buffered by the rejected translation") == std::string::npos);
    CHECK(terminal.find("response.failed") != std::string::npos);
    const auto failed = store.events_after(sink.id());
    CHECK(failed && failed->head.status == response_status::failed);
    CHECK(failed && failed->events.size() == failed->head.next_sequence_number);
    if (failed) {
        for (std::size_t index = 0; index < failed->events.size(); ++index) {
            CHECK(failed->events[index].at("sequence_number") == index);
        }
    }
}

}  // namespace

int main() try {
    test_text_reasoning_reconciliation_and_persistence();
    test_interleaved_namespace_custom_and_local_shell_tools();
    test_partial_tool_name_is_buffered_until_arguments();
    test_custom_argument_boundary_ignores_nested_string_syntax();
    test_checkpoint_failure_and_cancellation_contract();
    test_tombstoned_parent_does_not_revise_active_child();
    test_journaled_incomplete_and_failed_terminals();
    test_translated_update_failure_preserves_a_contiguous_terminal();
    test_translation_failure_poisoning_terminalizes_from_the_last_checkpoint();
    if (failures != 0) {
        std::cerr << failures << " server generation adapter checks failed\n";
        return 1;
    }
    std::cout << "llama-responses server generation adapter checks passed\n";
    return 0;
} catch (const std::exception & error) {
    std::cerr << "server generation adapter checks threw: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "server generation adapter checks threw an unknown exception\n";
    return 1;
}
