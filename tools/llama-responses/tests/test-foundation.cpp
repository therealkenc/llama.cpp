#include "hosted-tools.h"
#include "response-store.h"
#include "response-types.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <variant>

using namespace llama_responses;

// These declarations intentionally have external linkage: the focused test
// translation units are aggregated into this executable's single main().
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_codex_models();
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_generation();
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_protocol();

namespace {

int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

class fake_web_search_provider final : public hosted_tool_provider {
  public:
    hosted_tool_kind kind() const noexcept override { return hosted_tool_kind::web_search; }

    hosted_tool_capabilities capabilities() const override {
        hosted_tool_capabilities result;
        result.available  = true;
        result.operations = { "search" };
        return result;
    }

    hosted_tool_result invoke(const hosted_tool_request & request,
                              const hosted_tool_context & /*context*/,
                              hosted_tool_event_sink * /*events*/) override {
        hosted_tool_result result;
        if (!std::holds_alternative<web_search_request>(request)) {
            result.error_code = "wrong_request_kind";
            return result;
        }
        result.ok = true;
        result.content.push_back(hosted_tool_content{
            hosted_tool_content::kind::text,
            "fixture result",
            "",
            "",
            "",
        });
        return result;
    }
};

response_state make_state(const char * response_value, const char * item_value) {
    response_state state;
    state.id    = response_id(response_value);
    state.model = "fixture-model";

    response_output_item item;
    item.id            = item_id(item_value);
    item.type          = "message";
    item.value["type"] = "message";
    state.output.push_back(std::move(item));
    return state;
}

common_json make_input_item(const char * id) {
    return {
        { "id",   id        },
        { "type", "message" },
        { "role", "user"    },
    };
}

common_json render_first_output(const response_state & state) {
    const response_output_item & output = state.output.at(0);
    common_json                  result = output.value;
    result["id"]                        = output.id.str();
    result["type"]                      = output.type;
    const call_id call                  = output.call.value_or(call_id());
    if (!call.empty()) {
        result["call_id"] = call.str();
    }
    return result;
}

void test_types() {
    CHECK(std::string(response_status_name(response_status::in_progress)) == "in_progress");
    CHECK(response_status_can_transition(response_status::queued, response_status::in_progress));
    CHECK(response_status_can_transition(response_status::in_progress, response_status::completed));
    CHECK(response_status_can_transition(response_status::in_progress, response_status::in_progress));
    CHECK(!response_status_can_transition(response_status::completed, response_status::in_progress));
    CHECK(!response_status_can_transition(response_status::completed, response_status::completed));
    CHECK(response_status_is_terminal(response_status::incomplete));
    CHECK(std::string(response_event_type_name(response_event_type::output_text_delta)) ==
          "response.output_text.delta");

    response_id response("resp_fixture");
    item_id     item("msg_fixture");
    CHECK(response.str() == "resp_fixture");
    CHECK(item.str() == "msg_fixture");
}

void test_store() {
    in_memory_response_store store;
    response_state           first = make_state("resp_one", "msg_one");

    CHECK(store.create(first) == store_write_result::stored);
    CHECK(store.create(first) == store_write_result::already_exists);
    CHECK(store.size() == 1);

    auto stored = store.find(response_id("resp_one"));
    CHECK(stored.has_value());
    if (!stored) {
        return;
    }
    CHECK(stored->revision == 1);

    auto item = store.find_item(item_id("msg_one"));
    CHECK(item.has_value());
    if (!item) {
        return;
    }
    CHECK(item->owner == response_id("resp_one"));

    response_state conflicting = make_state("resp_two", "msg_one");
    CHECK(store.create(conflicting) == store_write_result::item_id_conflict);

    response_state stale        = *stored;
    stored->status              = response_status::completed;
    stored->usage.output_tokens = 4;
    CHECK(store.replace(*stored) == store_write_result::stored);
    CHECK(store.replace(stale) == store_write_result::stale_revision);

    auto completed = store.find(response_id("resp_one"));
    CHECK(completed.has_value());
    if (!completed) {
        return;
    }
    CHECK(completed->revision == 2);
    CHECK(completed->usage.output_tokens == 4);
    completed->status = response_status::in_progress;
    CHECK(store.replace(*completed) == store_write_result::invalid_transition);

    CHECK(store.erase(response_id("resp_one")));
    CHECK(!store.erase(response_id("resp_one")));
    CHECK(!store.find(response_id("resp_one")).has_value());
    CHECK(!store.find_item(item_id("msg_one")).has_value());
    CHECK(!store.materialize_input_items(response_id("resp_one")).has_value());
    CHECK(!store.materialize_continuation_context(response_id("resp_one")).has_value());
    CHECK(store.size() == 0);

    response_state tombstoned_item_conflict = make_state("resp_after_delete", "msg_one");
    CHECK(store.create(tombstoned_item_conflict) == store_write_result::item_id_conflict);

    // Each graph node owns only its direct input. Materialized input and
    // continuation views are derived from immutable parent links.
    in_memory_response_store lineage;
    response_state           lineage_root   = make_state("resp_lineage_root", "msg_lineage_root_output");
    response_state           lineage_middle = make_state("resp_lineage_middle", "msg_lineage_middle_output");
    response_state           lineage_leaf   = make_state("resp_lineage_leaf", "msg_lineage_leaf_output");
    response_state           lineage_branch = make_state("resp_lineage_branch", "msg_lineage_branch_output");
    const common_json        root_input     = make_input_item("msg_lineage_root_input");
    const common_json        middle_input   = make_input_item("msg_lineage_middle_input");
    const common_json        leaf_input     = make_input_item("msg_lineage_leaf_input");
    const common_json        branch_input   = make_input_item("msg_lineage_branch_input");

    lineage_middle.output.at(0).type  = "function_call_output";
    lineage_middle.output.at(0).call  = call_id("call_lineage_middle");
    lineage_middle.output.at(0).value = {
        { "type",    "function_call_output" },
        { "call_id", "call_lineage_middle"  },
        { "output",  common_json::array({
                        {
                            { "type", "input_text" },
                            { "text", "rich tool result" },
                        },
                        {
                            { "type", "input_image" },
                            { "image_url", "data:image/png;base64,aW1hZ2U=" },
                        },
                        {
                            { "type", "input_file" },
                            { "filename", "notes.txt" },
                            { "file_data", "bm90ZXM=" },
                        },
                    })   },
    };
    const common_json resolved_middle_output = render_first_output(lineage_middle);

    lineage_root.input_items.push_back(root_input);
    lineage_middle.previous_response = lineage_root.id;
    lineage_middle.input_items.push_back(middle_input);
    lineage_leaf.previous_response = lineage_middle.id;
    lineage_leaf.input_items.push_back(leaf_input);
    lineage_branch.previous_response = lineage_root.id;
    lineage_branch.input_items.push_back(branch_input);

    for (response_state * state : { &lineage_root, &lineage_middle, &lineage_leaf, &lineage_branch }) {
        state->status       = response_status::completed;
        state->completed_at = 101;
    }

    CHECK(lineage.create(lineage_root) == store_write_result::stored);
    CHECK(lineage.create(lineage_middle) == store_write_result::stored);
    CHECK(lineage.create(lineage_leaf) == store_write_result::stored);
    CHECK(lineage.create(lineage_branch) == store_write_result::stored);

    const auto middle_inputs = lineage.materialize_input_items(lineage_middle.id);
    CHECK(middle_inputs && middle_inputs->size() == 3);
    if (middle_inputs && middle_inputs->size() == 3) {
        CHECK(middle_inputs->at(0).at("id") == "msg_lineage_root_input");
        CHECK(middle_inputs->at(1).at("id") == "msg_lineage_root_output");
        CHECK(middle_inputs->at(2).at("id") == "msg_lineage_middle_input");
    }
    const auto middle_continuation = lineage.materialize_continuation_context(lineage_middle.id);
    CHECK(middle_continuation && middle_continuation->size() == 4);
    if (middle_continuation && middle_continuation->size() == 4) {
        CHECK(middle_continuation->at(3).at("id") == "msg_lineage_middle_output");
    }

    const auto leaf_inputs = lineage.materialize_input_items(lineage_leaf.id);
    CHECK(leaf_inputs && leaf_inputs->size() == 5);
    const auto branch_inputs = lineage.materialize_input_items(lineage_branch.id);
    CHECK(branch_inputs && branch_inputs->size() == 3);
    if (branch_inputs && branch_inputs->size() == 3) {
        CHECK(branch_inputs->at(2).at("id") == "msg_lineage_branch_input");
    }

    // A resolved item_reference is a direct input contribution. It remains
    // useful after its original owner becomes publicly invisible.
    response_state resolved_reference = make_state("resp_resolved_reference", "msg_resolved_reference_output");
    resolved_reference.status         = response_status::completed;
    resolved_reference.completed_at   = 101;
    resolved_reference.input_items.push_back(resolved_middle_output);
    CHECK(lineage.create(resolved_reference) == store_write_result::stored);

    // Preserve the race boundary used by route preparation: materialization
    // wins before deletion, then child persistence may attach to the retained
    // internal tombstone even though new public parent lookups fail.
    const auto prepared_middle = lineage.materialize_continuation_context(lineage_middle.id);
    CHECK(prepared_middle.has_value());
    CHECK(lineage.erase(lineage_middle.id));
    CHECK(lineage.find(lineage_root.id).has_value());
    CHECK(!lineage.find(lineage_middle.id).has_value());
    CHECK(!lineage.find_item(item_id("msg_lineage_middle_output")).has_value());
    CHECK(!lineage.materialize_input_items(lineage_middle.id).has_value());
    CHECK(!lineage.materialize_continuation_context(lineage_middle.id).has_value());
    CHECK(!lineage.erase(lineage_middle.id));
    CHECK(lineage.size() == 4);
    CHECK(lineage.materialize_input_items(lineage_leaf.id) == leaf_inputs);
    CHECK(lineage.materialize_input_items(lineage_branch.id) == branch_inputs);

    const auto preserved_reference = lineage.materialize_input_items(resolved_reference.id);
    CHECK(preserved_reference && preserved_reference->size() == 1);
    if (preserved_reference && preserved_reference->size() == 1) {
        CHECK(preserved_reference->at(0) == resolved_middle_output);
    }

    response_state raced_child    = make_state("resp_raced_child", "msg_raced_child_output");
    raced_child.status            = response_status::completed;
    raced_child.completed_at      = 101;
    raced_child.previous_response = lineage_middle.id;
    raced_child.input_items.push_back(make_input_item("msg_raced_child_input"));
    CHECK(lineage.create(raced_child) == store_write_result::stored);
    const auto raced_inputs = lineage.materialize_input_items(raced_child.id);
    CHECK(prepared_middle && raced_inputs && raced_inputs->size() == prepared_middle->size() + 1);
    if (prepared_middle && raced_inputs && raced_inputs->size() == prepared_middle->size() + 1) {
        for (std::size_t index = 0; index < prepared_middle->size(); ++index) {
            CHECK(raced_inputs->at(index) == prepared_middle->at(index));
        }
        CHECK(raced_inputs->back().at("id") == "msg_raced_child_input");
    }

    response_state tombstone_conflict = make_state("resp_tombstone_conflict", "msg_lineage_middle_output");
    tombstone_conflict.status         = response_status::completed;
    tombstone_conflict.completed_at   = 101;
    CHECK(lineage.create(tombstone_conflict) == store_write_result::item_id_conflict);

    CHECK(lineage.erase(lineage_root.id));
    CHECK(!lineage.find(lineage_root.id).has_value());
    CHECK(lineage.size() == 4);
    CHECK(lineage.materialize_input_items(lineage_leaf.id) == leaf_inputs);
    CHECK(lineage.materialize_input_items(lineage_branch.id) == branch_inputs);

    // Capacity applies to publicly visible resources, not the immutable graph
    // storage needed by their descendants. Automatic retirement therefore
    // hides the oldest terminal root without detaching or copying its lineage.
    in_memory_response_store bounded(2);
    response_state           bounded_root   = make_state("resp_bounded_root", "msg_bounded_root_output");
    response_state           bounded_middle = make_state("resp_bounded_middle", "msg_bounded_middle_output");
    response_state           bounded_leaf   = make_state("resp_bounded_leaf", "msg_bounded_leaf_output");
    bounded_root.input_items.push_back(make_input_item("msg_bounded_root_input"));
    bounded_middle.previous_response = bounded_root.id;
    bounded_middle.input_items.push_back(make_input_item("msg_bounded_middle_input"));
    bounded_leaf.previous_response = bounded_middle.id;
    bounded_leaf.input_items.push_back(make_input_item("msg_bounded_leaf_input"));
    for (response_state * state : { &bounded_root, &bounded_middle, &bounded_leaf }) {
        state->status       = response_status::completed;
        state->completed_at = 101;
    }
    CHECK(bounded.create(bounded_root) == store_write_result::stored);
    CHECK(bounded.create(bounded_middle) == store_write_result::stored);
    CHECK(bounded.create(bounded_leaf) == store_write_result::stored);
    CHECK(bounded.size() == 2);
    CHECK(!bounded.find(bounded_root.id).has_value());
    CHECK(!bounded.find_item(item_id("msg_bounded_root_output")).has_value());
    CHECK(!bounded.materialize_input_items(bounded_root.id).has_value());
    const auto bounded_leaf_inputs = bounded.materialize_input_items(bounded_leaf.id);
    CHECK(bounded_leaf_inputs && bounded_leaf_inputs->size() == 5);
    if (bounded_leaf_inputs && bounded_leaf_inputs->size() == 5) {
        CHECK(bounded_leaf_inputs->at(0).at("id") == "msg_bounded_root_input");
        CHECK(bounded_leaf_inputs->at(1).at("id") == "msg_bounded_root_output");
        CHECK(bounded_leaf_inputs->at(4).at("id") == "msg_bounded_leaf_input");
    }

    response_state invalid;
    CHECK(store.create(invalid) == store_write_result::invalid_state);
}

void test_hosted_tools() {
    hosted_tool_registry registry;
    hosted_tool_context  context;
    context.response = response_id("resp_fixture");
    context.call     = call_id("call_fixture");

    web_search_request search;
    search.queries.push_back("llama.cpp");
    hosted_tool_request request = search;

    CHECK(!registry.available(hosted_tool_kind::web_search));
    hosted_tool_result unavailable = registry.invoke(request, context);
    CHECK(!unavailable.ok);
    CHECK(unavailable.error_code == "hosted_tool_provider_unavailable");

    registry.install(std::make_shared<fake_web_search_provider>());
    CHECK(registry.available(hosted_tool_kind::web_search));
    hosted_tool_result result = registry.invoke(request, context);
    CHECK(result.ok);
    CHECK(result.content.size() == 1);
    CHECK(result.content[0].text == "fixture result");
    CHECK(!registry.available(hosted_tool_kind::shell));
}

}  // namespace

int main() try {
    test_types();
    test_store();
    test_hosted_tools();
    failures += test_codex_models();
    failures += test_generation();
    failures += test_protocol();

    if (failures != 0) {
        std::cerr << failures << " foundation checks failed\n";
        return 1;
    }
    std::cout << "llama-responses foundation checks passed\n";
    return 0;
} catch (const std::exception & error) {
    std::cerr << "llama-responses foundation checks threw: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "llama-responses foundation checks threw an unknown exception\n";
    return 1;
}
