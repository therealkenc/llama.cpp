#include "hosted-tools.h"
#include "response-store.h"
#include "response-types.h"

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
    common_json result = state.output.at(0).value;
    result["id"]       = state.output.at(0).id.str();
    result["type"]     = state.output.at(0).type;
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
    CHECK(!store.find_item(item_id("msg_one")).has_value());
    CHECK(store.size() == 0);

    in_memory_response_store bounded(2);
    response_state           response_a = make_state("resp_a", "msg_a");
    response_state           response_b = make_state("resp_b", "msg_b");
    response_state           response_c = make_state("resp_c", "msg_c");
    response_state           response_d = make_state("resp_d", "msg_d");
    response_b.previous_response        = response_a.id;
    response_c.previous_response        = response_b.id;
    response_d.previous_response        = response_c.id;
    CHECK(bounded.create(response_a) == store_write_result::stored);
    CHECK(bounded.create(response_b) == store_write_result::stored);
    CHECK(bounded.create(response_c) == store_write_result::stored);
    CHECK(!bounded.find(response_id("resp_a")).has_value());
    CHECK(!bounded.find_item(item_id("msg_a")).has_value());
    CHECK(bounded.size() == 2);
    auto response_b_stored = bounded.find(response_b.id);
    CHECK(response_b_stored.has_value());
    if (!response_b_stored || !response_b_stored->detached_context) {
        CHECK(false);
        return;
    }
    CHECK(response_b_stored->detached_context->size() == 1);
    CHECK(response_b_stored->detached_context->at(0).at("id") == "msg_a");

    CHECK(bounded.create(response_d) == store_write_result::stored);
    auto response_c_stored = bounded.find(response_c.id);
    CHECK(response_c_stored.has_value());
    if (!response_c_stored || !response_c_stored->detached_context) {
        CHECK(false);
        return;
    }
    CHECK(response_c_stored->detached_context->size() == 2);
    CHECK(response_c_stored->detached_context->at(0).at("id") == "msg_a");
    CHECK(response_c_stored->detached_context->at(1).at("id") == "msg_b");

    CHECK(bounded.erase(response_c.id));
    auto response_d_stored = bounded.find(response_d.id);
    CHECK(response_d_stored.has_value());
    if (!response_d_stored || !response_d_stored->detached_context) {
        CHECK(false);
        return;
    }
    CHECK(response_d_stored->detached_context->size() == 3);
    CHECK(response_d_stored->detached_context->at(2).at("id") == "msg_c");

    // Deleting an interior node must carry the complete materialized lineage
    // into its child even while the grandparent remains attached.
    in_memory_response_store lineage;
    response_state           lineage_root   = make_state("resp_lineage_root", "msg_lineage_root_output");
    response_state           lineage_middle = make_state("resp_lineage_middle", "msg_lineage_middle_output");
    response_state           lineage_leaf   = make_state("resp_lineage_leaf", "msg_lineage_leaf_output");
    const common_json        root_input     = make_input_item("msg_lineage_root_input");
    const common_json        middle_input   = make_input_item("msg_lineage_middle_input");
    const common_json        leaf_input     = make_input_item("msg_lineage_leaf_input");

    lineage_root.input_items.push_back(root_input);
    lineage_root.continuation_input_items.push_back(root_input);

    lineage_middle.previous_response = lineage_root.id;
    lineage_middle.input_items       = lineage_root.input_items;
    lineage_middle.input_items.push_back(render_first_output(lineage_root));
    lineage_middle.input_items.push_back(middle_input);
    lineage_middle.continuation_input_items.push_back(middle_input);

    lineage_leaf.previous_response = lineage_middle.id;
    lineage_leaf.input_items       = lineage_middle.input_items;
    lineage_leaf.input_items.push_back(render_first_output(lineage_middle));
    lineage_leaf.input_items.push_back(leaf_input);
    lineage_leaf.continuation_input_items.push_back(leaf_input);

    CHECK(lineage.create(lineage_root) == store_write_result::stored);
    CHECK(lineage.create(lineage_middle) == store_write_result::stored);
    CHECK(lineage.create(lineage_leaf) == store_write_result::stored);
    CHECK(lineage.erase(lineage_middle.id));
    CHECK(lineage.find(lineage_root.id).has_value());
    const auto detached_leaf = lineage.find(lineage_leaf.id);
    CHECK(detached_leaf && detached_leaf->detached_context.has_value());
    if (detached_leaf && detached_leaf->detached_context) {
        CHECK(detached_leaf->detached_context->size() == 4);
        CHECK(detached_leaf->detached_context->at(0).at("id") == "msg_lineage_root_input");
        CHECK(detached_leaf->detached_context->at(1).at("id") == "msg_lineage_root_output");
        CHECK(detached_leaf->detached_context->at(2).at("id") == "msg_lineage_middle_input");
        CHECK(detached_leaf->detached_context->at(3).at("id") == "msg_lineage_middle_output");
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
