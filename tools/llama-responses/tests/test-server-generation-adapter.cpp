#include "chat.h"
#include "generation.h"
#include "json.h"
#include "protocol-codec.h"
#include "response-store.h"
#include "response-types.h"
#include "server-generation-adapter.h"
#include "server-generation.h"

#include <cstddef>
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
    context.input_items              = common_json::array({
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
    context.continuation_input_items = context.input_items;
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
        CHECK(stored->revision == 4);
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

class failing_store final : public response_store {
  public:
    store_write_result create(response_state /*state*/) override { return store_write_result::invalid_state; }

    store_write_result replace(response_state /*state*/) override { return store_write_result::invalid_state; }

    std::optional<response_state> find(const response_id & /*id*/) const override { return std::nullopt; }

    std::optional<stored_response_item> find_item(const item_id & /*id*/) const override { return std::nullopt; }

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

void test_lineage_detachment_revision_is_reconciled() {
    in_memory_response_store store;

    native_server_generation_sink parent(fixture_context("detach_parent"), "adapter_detach_parent", false, &store);
    parent.accept(server_generation_started{});
    parent.accept(server_generation_message_snapshot{ common_chat_msg{} });
    parent.accept(server_generation_completed{ fixture_usage(), 101 });

    generation_response_context child_context = fixture_context("detach_child");
    child_context.previous_response           = parent.id();
    native_server_generation_sink child(std::move(child_context), "adapter_detach_child", true, &store);
    child.accept(server_generation_started{});

    CHECK(store.erase(parent.id()));
    const auto detached = store.find(child.id());
    CHECK(detached.has_value());
    CHECK(detached && detached->revision == 2);
    CHECK(detached && detached->detached_context.has_value());

    common_chat_msg_diff text;
    text.content_delta       = "still streaming";
    const std::string stream = child.accept(server_generation_message_deltas{ { text } });
    CHECK(stream.find("response_store_error") == std::string::npos);
    CHECK(!child.storage_failed());

    const auto updated = store.find(child.id());
    CHECK(updated.has_value());
    CHECK(updated && updated->revision == 3);
    CHECK(updated && updated->detached_context.has_value());
}

}  // namespace

int main() try {
    test_text_reasoning_reconciliation_and_persistence();
    test_interleaved_namespace_custom_and_local_shell_tools();
    test_partial_tool_name_is_buffered_until_arguments();
    test_checkpoint_failure_and_cancellation_contract();
    test_lineage_detachment_revision_is_reconciled();
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
