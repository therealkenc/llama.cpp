#include "chat.h"
#include "json.h"
#include "server-common.h"
#include "server-generation-internal.h"
#include "server-generation.h"
#include "server-queue.h"
#include "server-task.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

void test_partial_mapping() {
    server_task_result_cmpl_partial begin;
    begin.is_begin = true;
    auto updates   = server_generation_updates_from_result(begin, true);
    CHECK(updates.size() == 1);
    CHECK(std::holds_alternative<server_generation_started>(updates.at(0)));

    server_task_result_cmpl_partial partial;
    partial.is_progress = true;

    common_chat_msg_diff reasoning;
    reasoning.reasoning_content_delta = "think";
    partial.oaicompat_msg_diffs.push_back(reasoning);

    common_chat_msg_diff tool;
    tool.tool_call_index      = 1;
    tool.tool_call_delta.name = "apply_patch";
    tool.tool_call_delta.id   = "call_model";
    partial.oaicompat_msg_diffs.push_back(tool);
    updates = server_generation_updates_from_result(partial, false);
    CHECK(updates.size() == 2);
    CHECK(std::holds_alternative<server_generation_progress>(updates.at(0)));

    const auto & deltas = std::get<server_generation_message_deltas>(updates.at(1));
    CHECK(deltas.deltas.size() == 2);
    CHECK(deltas.deltas.at(0).reasoning_content_delta == "think");
    CHECK(deltas.deltas.at(1).tool_call_index == 1);
}

void test_final_mapping() {
    server_task_result_cmpl_final final;
    final.n_prompt_tokens       = 41;
    final.n_prompt_tokens_cache = 11;
    final.n_decoded             = 7;
    final.stop                  = STOP_TYPE_LIMIT;

    common_chat_msg_diff text;
    text.content_delta = "answer";
    final.oaicompat_msg_diffs.push_back(text);
    final.oaicompat_msg.content = "answer";

    auto updates = server_generation_updates_from_result(final, true);
    CHECK(updates.size() == 4);
    CHECK(std::holds_alternative<server_generation_started>(updates.at(0)));
    CHECK(std::holds_alternative<server_generation_message_deltas>(updates.at(1)));
    const auto & snapshot = std::get<server_generation_message_snapshot>(updates.at(2));
    CHECK(snapshot.message.content == "answer");

    const auto & incomplete = std::get<server_generation_incomplete>(updates.at(3));
    CHECK(incomplete.reason == "max_output_tokens");
    CHECK(incomplete.usage.input_tokens == 41);
    CHECK(incomplete.usage.cached_input_tokens == 11);
    CHECK(incomplete.usage.output_tokens == 7);
    CHECK(server_generation_update_is_terminal(updates.at(3)));

    final.stop = STOP_TYPE_EOS;
    updates    = server_generation_updates_from_result(final, false);
    CHECK(updates.size() == 3);
    const auto & completed = std::get<server_generation_completed>(updates.at(2));
    CHECK(completed.completed_at != 0);
    CHECK(completed.usage.output_tokens == 7);

    server_task_result_cmpl_final unparsed;
    unparsed.content = "raw parser fallback";
    updates          = server_generation_updates_from_result(unparsed, false);
    CHECK(updates.size() == 2);
    const auto & fallback = std::get<server_generation_message_snapshot>(updates.at(0));
    CHECK(fallback.message.role == "assistant");
    CHECK(fallback.message.content == "raw parser fallback");
}

void test_error_mapping() {
    server_task_result_error error;
    error.err_type = ERROR_TYPE_UNAVAILABLE;
    error.err_msg  = "busy";

    const auto updates = server_generation_updates_from_result(error, true);
    CHECK(updates.size() == 2);
    const auto & failed = std::get<server_generation_failed>(updates.at(1));
    CHECK(failed.error.code == "server_overloaded");
    CHECK(failed.error.message == "busy");
    CHECK(!failed.usage.has_value());
    CHECK(server_generation_update_is_terminal(updates.at(1)));
}

class recording_sink final : public server_generation_sink {
  public:
    std::string accept(const server_generation_update & update) override {
        received.push_back(update);
        return "opaque-frame-" + std::to_string(received.size()) + ';';
    }

    common_json snapshot() const override {
        return {
            { "updates", received.size() },
        };
    }

    std::vector<server_generation_update> received;
};

void test_sink_contract_keeps_projection_opaque() {
    recording_sink    sink;
    const std::string first  = sink.accept(server_generation_started{});
    const std::string second = sink.accept(server_generation_completed{});

    CHECK(first == "opaque-frame-1;");
    CHECK(second == "opaque-frame-2;");
    CHECK(sink.snapshot().at("updates") == 2);
}

void test_abandoned_projection_terminalizes_once_after_last_copy() {
    auto sink = std::make_shared<recording_sink>();
    {
        server_generation_projection projection(sink);
        {
            // This copy models std::function copying the streaming callback;
            // the test specifically verifies shared projection lifetime.
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            server_generation_projection copied = projection;
            CHECK(copied.enabled());
        }
        // std::function copies its streaming callback. A temporary callback
        // copy must not cancel a still-live projection.
        CHECK(sink->received.empty());
    }

    CHECK(sink->received.size() == 2U);
    CHECK(std::holds_alternative<server_generation_started>(sink->received.at(0)));
    CHECK(std::holds_alternative<server_generation_cancelled>(sink->received.at(1)));
}

void test_queue_shutdown_releases_sleep_waiters() {
    server_queue queue;
    queue.on_update_slots([] {});

    std::thread loop([&queue] { queue.start_loop(0); });
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!queue.is_sleeping() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!queue.is_sleeping()) {
        queue.terminate();
        loop.join();
        CHECK(false && "server queue did not enter its sleeping state");
        return;
    }

    queue.terminate();
    loop.join();
    CHECK(!queue.is_sleeping());
    // A response worker may reach this gate while the main loop is unwinding.
    // It must return rather than waiting forever on a state no thread owns.
    queue.wait_until_no_sleep();
}

}  // namespace

int main() try {
    test_partial_mapping();
    test_final_mapping();
    test_error_mapping();
    test_sink_contract_keeps_projection_opaque();
    test_abandoned_projection_terminalizes_once_after_last_copy();
    test_queue_shutdown_releases_sleep_waiters();
    if (failures != 0) {
        std::cerr << failures << " neutral generation sink checks failed\n";
        return 1;
    }
    std::cout << "neutral generation sink checks passed\n";
    return 0;
} catch (const std::exception & error) {
    std::cerr << "neutral generation sink checks threw: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "neutral generation sink checks threw an unknown exception\n";
    return 1;
}
