#include "generation.h"
#include "json.h"
#include "response-types.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace llama_responses;

// This is the cross-translation-unit entry point which will be called by the
// foundation test executable when the generation slice is wired into CMake.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_generation();

namespace {

int failures = 0;

#define GENERATION_CHECK(condition)                                                         \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

response_usage fixture_usage() {
    response_usage usage;
    usage.input_tokens            = 19;
    usage.cached_input_tokens     = 7;
    usage.output_tokens           = 11;
    usage.reasoning_output_tokens = 3;
    return usage;
}

generation_response_context fixture_context(const std::string & suffix) {
    generation_response_context context;
    context.model      = "fixture-model";
    context.created_at = 100;
    context.request    = {
        { "model",       context.model             },
        { "input",       "fixture input"           },
        { "store",       true                      },
        { "temperature", 0.25                      },
        { "metadata",    { { "fixture", suffix } } },
    };
    context.input_items              = common_json::array({
        {
         { "id", "msg_input_" + suffix },
         { "type", "message" },
         { "role", "user" },
         { "content", common_json::array({
                             {
                                 { "type", "input_text" },
                                 { "text", "fixture input" },
                             },
                         }) },
         },
    });
    context.continuation_input_items = context.input_items;
    return context;
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

void drain(generation_port & port, native_response_state_machine & machine) {
    generation_request request;
    request.model      = "fixture-model";
    request.prompt     = "prepared prompt";
    request.parameters = {
        { "temperature", 0.25 }
    };
    std::unique_ptr<generation_session> session = port.start(request);
    GENERATION_CHECK(session != nullptr);
    if (!session) {
        return;
    }
    machine.start();
    while (const std::optional<generation_update> update = session->next()) {
        machine.apply(*update);
    }
}

void test_scripted_interleaving() {
    const response_usage     usage = fixture_usage();
    scripted_generation_port port({
        generation_started{},
        generation_progress{},
        generation_reasoning_delta{ "inspect" },
        generation_reasoning_delta{ " carefully" },
        generation_text_delta{ "hello " },
        generation_text_delta{ "world" },
        // Deliberately start parser index 9 before index 2. Wire output order
        // follows appearance, while subsequent deltas correlate by parser id.
        generation_tool_call_started{ 9, generation_tool_kind::function, "lookup", "tool_a" },
        generation_tool_call_started{ 2, generation_tool_kind::custom, "apply_patch", "tool_b" },
        generation_tool_call_delta{ 9, R"({"query":)" },
        generation_tool_call_delta{ 2, "*** Begin" },
        generation_tool_call_delta{ 9, R"("llama"})" },
        generation_tool_call_delta{ 2, " Patch" },
        generation_usage_update{ usage },
        generation_completed{ usage, 101 },
    });

    counter_generation_id_source  ids("interleave");
    native_response_state_machine machine(fixture_context("interleave"), ids);
    drain(port, machine);

    GENERATION_CHECK(machine.terminal());
    const common_json snapshot = machine.snapshot();
    GENERATION_CHECK(snapshot.at("status") == "completed");
    GENERATION_CHECK(snapshot.at("completed_at") == 101);
    GENERATION_CHECK(snapshot.at("output_text") == "hello world");
    GENERATION_CHECK(snapshot.at("usage").at("input_tokens") == 19);
    GENERATION_CHECK(snapshot.at("usage").at("input_tokens_details").at("cached_tokens") == 7);
    GENERATION_CHECK(snapshot.at("usage").at("output_tokens_details").at("reasoning_tokens") == 3);
    GENERATION_CHECK(snapshot.at("metadata").at("fixture") == "interleave");
    GENERATION_CHECK(machine.state().continuation_input_items == machine.state().input_items);

    const common_json & output = snapshot.at("output");
    GENERATION_CHECK(output.size() == 4);
    if (output.size() != 4) {
        return;
    }
    GENERATION_CHECK(output.at(0).at("type") == "reasoning");
    GENERATION_CHECK(output.at(1).at("type") == "message");
    GENERATION_CHECK(output.at(2).at("type") == "function_call");
    GENERATION_CHECK(output.at(3).at("type") == "custom_tool_call");
    GENERATION_CHECK(output.at(2).at("arguments") == R"({"query":"llama"})");
    GENERATION_CHECK(output.at(3).at("input") == "*** Begin Patch");

    const std::string function_item_id = output.at(2).at("id").get<std::string>();
    const std::string function_call_id = output.at(2).at("call_id").get<std::string>();
    const std::string custom_item_id   = output.at(3).at("id").get<std::string>();
    const std::string custom_call_id   = output.at(3).at("call_id").get<std::string>();
    GENERATION_CHECK(function_item_id.rfind("fc_", 0) == 0);
    GENERATION_CHECK(function_call_id == "call_tool_a");
    GENERATION_CHECK(function_item_id != function_call_id);
    GENERATION_CHECK(custom_item_id.rfind("ctc_", 0) == 0);
    GENERATION_CHECK(custom_call_id == "call_tool_b");
    GENERATION_CHECK(custom_item_id != custom_call_id);

    const std::vector<common_json> events = machine.rendered_events();
    GENERATION_CHECK(!events.empty());
    for (std::size_t index = 0; index < events.size(); ++index) {
        GENERATION_CHECK(events[index].at("sequence_number").get<std::size_t>() == index);
    }

    const auto function_deltas = events_named(events, "response.function_call_arguments.delta");
    const auto custom_deltas   = events_named(events, "response.custom_tool_call_input.delta");
    GENERATION_CHECK(function_deltas.size() == 2);
    GENERATION_CHECK(custom_deltas.size() == 2);
    for (const common_json & event : function_deltas) {
        GENERATION_CHECK(event.at("item_id") == function_item_id);
        GENERATION_CHECK(event.at("output_index") == 2);
    }
    for (const common_json & event : custom_deltas) {
        GENERATION_CHECK(event.at("item_id") == custom_item_id);
        GENERATION_CHECK(event.at("output_index") == 3);
    }

    const auto done_events = events_named(events, "response.output_item.done");
    GENERATION_CHECK(done_events.size() == output.size());
    if (done_events.size() == output.size()) {
        for (std::size_t index = 0; index < done_events.size(); ++index) {
            GENERATION_CHECK(done_events[index].at("output_index").get<std::size_t>() == index);
            GENERATION_CHECK(done_events[index].at("item") == output.at(index));
        }
    }

    // The synchronous body and the terminal SSE projection are views of the
    // same authoritative response_state, not independently assembled objects.
    GENERATION_CHECK(events.back().at("type") == "response.completed");
    GENERATION_CHECK(events.back().at("response") == snapshot);
}

common_json run_terminal(const generation_update & terminal_update, const std::string & suffix) {
    scripted_generation_port      port({ terminal_update });
    counter_generation_id_source  ids(suffix);
    native_response_state_machine machine(fixture_context(suffix), ids);
    drain(port, machine);
    GENERATION_CHECK(machine.terminal());
    const std::vector<common_json> events = machine.rendered_events();
    GENERATION_CHECK(!events.empty());
    GENERATION_CHECK(events.back().at("response") == machine.snapshot());
    return common_json{
        { "snapshot", machine.snapshot() },
        { "events",   events             },
    };
}

void test_terminal_states() {
    const response_usage usage = fixture_usage();

    common_json incomplete = run_terminal(generation_incomplete{ usage, "max_output_tokens" }, "incomplete");
    GENERATION_CHECK(incomplete.at("snapshot").at("status") == "incomplete");
    GENERATION_CHECK(incomplete.at("snapshot").at("incomplete_details").at("reason") == "max_output_tokens");
    GENERATION_CHECK(incomplete.at("snapshot").at("usage").at("total_tokens") == 30);
    GENERATION_CHECK(incomplete.at("events").back().at("type") == "response.incomplete");

    response_error error;
    error.code         = "fixture_failure";
    error.message      = "generation failed";
    error.param        = "input";
    common_json failed = run_terminal(generation_failed{ error, std::nullopt }, "failed");
    GENERATION_CHECK(failed.at("snapshot").at("status") == "failed");
    GENERATION_CHECK(failed.at("snapshot").at("usage").is_null());
    GENERATION_CHECK(failed.at("snapshot").at("error").at("code") == "fixture_failure");
    GENERATION_CHECK(failed.at("events").at(failed.at("events").size() - 2).at("type") == "error");
    GENERATION_CHECK(failed.at("events").back().at("type") == "response.failed");

    common_json cancelled = run_terminal(generation_cancelled{ usage }, "cancelled");
    GENERATION_CHECK(cancelled.at("snapshot").at("status") == "cancelled");
    GENERATION_CHECK(cancelled.at("snapshot").at("usage").at("output_tokens") == 11);
    GENERATION_CHECK(cancelled.at("events").back().at("type") == "response.cancelled");
}

void test_continuation_input_fallback() {
    generation_response_context context = fixture_context("continuation");
    context.continuation_input_items    = nullptr;
    counter_generation_id_source  ids("continuation");
    native_response_state_machine machine(std::move(context), ids);
    GENERATION_CHECK(machine.state().continuation_input_items == machine.state().input_items);
}

void test_scripted_cancellation() {
    scripted_generation_port port({
        generation_text_delta{ "not observed" },
        generation_completed{ fixture_usage(), 101 },
    });
    generation_request       request;
    request.model                               = "fixture-model";
    std::unique_ptr<generation_session> session = port.start(request);
    GENERATION_CHECK(session != nullptr);
    if (!session) {
        return;
    }
    session->request_cancel();
    GENERATION_CHECK(session->cancel_requested());
    const std::optional<generation_update> cancelled = session->next();
    GENERATION_CHECK(cancelled.has_value());
    GENERATION_CHECK(cancelled && std::holds_alternative<generation_cancelled>(*cancelled));
    GENERATION_CHECK(!session->next().has_value());

    counter_generation_id_source  ids("cancel-request");
    native_response_state_machine machine(fixture_context("cancel-request"), ids);
    if (cancelled) {
        machine.apply(*cancelled);
    }
    GENERATION_CHECK(machine.snapshot().at("status") == "cancelled");

    bool threw = false;
    try {
        machine.apply(generation_text_delta{ "too late" });
    } catch (const std::logic_error &) {
        threw = true;
    }
    GENERATION_CHECK(threw);
}

}  // namespace

// This function intentionally has external linkage; the existing foundation
// main() can call it once this isolated slice is added to the build.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_generation() {
    test_scripted_interleaving();
    test_terminal_states();
    test_continuation_input_fallback();
    test_scripted_cancellation();
    return failures;
}
