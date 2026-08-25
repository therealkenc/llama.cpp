#include "json.h"
#include "protocol-codec.h"
#include "response-service.h"
#include "response-store.h"
#include "response-types.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace llama_responses;

// This is the cross-translation-unit entry point called by test-foundation.cpp.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_protocol();

namespace {

int failures = 0;

#define PROTOCOL_CHECK(condition)                                                           \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

common_json make_request() {
    return common_json::parse(R"({
        "model":"fixture-model",
        "input":[
            {"role":"user","content":"first"},
            {"role":"user","content":[{"type":"input_text","text":"second"}]},
            {"role":"user","content":"third"}
        ],
        "temperature":0.25,
        "store":true,
        "metadata":{"fixture":"yes"}
    })");
}

common_json make_wire_response() {
    return common_json::parse(R"({
        "id":"resp_protocol",
        "object":"response",
        "created_at":123,
        "completed_at":124,
        "status":"completed",
        "model":"fixture-model",
        "output":[
            {
                "id":"msg_protocol",
                "type":"message",
                "status":"completed",
                "role":"assistant",
                "content":[{"type":"output_text","text":"hello","annotations":[]}]
            },
            {
                "id":"fc_protocol",
                "type":"function_call",
                "status":"completed",
                "call_id":"call_protocol",
                "name":"fixture",
                "arguments":"{}"
            }
        ],
        "usage":{
            "input_tokens":7,
            "input_tokens_details":{"cached_tokens":2,"cache_write_tokens":5},
            "output_tokens":3,
            "output_tokens_details":{"reasoning_tokens":1,"future_detail":7},
            "total_tokens":10
        },
        "metadata":{"fixture":"yes"},
        "future_wire_field":{"preserved":true}
    })");
}

void test_capture_and_render() {
    common_json request = make_request();
    common_json inputs  = capture_input_items(
        request, [](std::size_t index, const common_json &) { return item_id("msg_input_" + std::to_string(index)); });

    PROTOCOL_CHECK(inputs.size() == 3);
    PROTOCOL_CHECK(inputs[0].at("id").get<std::string>() == "msg_input_0");
    PROTOCOL_CHECK(inputs[0].at("type").get<std::string>() == "message");
    PROTOCOL_CHECK(inputs[0].at("content").is_array());
    PROTOCOL_CHECK(inputs[0].at("content")[0].at("text").get<std::string>() == "first");

    response_state state = capture_response_state(make_wire_response(), request, inputs);
    PROTOCOL_CHECK(state.id == response_id("resp_protocol"));
    PROTOCOL_CHECK(state.completed_at.value_or(0) == 124);
    PROTOCOL_CHECK(state.output.size() == 2);
    PROTOCOL_CHECK(state.output[1].id == item_id("fc_protocol"));
    PROTOCOL_CHECK(state.output[1].call.value_or(call_id()) == call_id("call_protocol"));
    PROTOCOL_CHECK(state.usage.cached_input_tokens == 2);

    response_state automatically_captured = capture_response_state(make_wire_response(), request);
    PROTOCOL_CHECK(automatically_captured.input_items.size() == 3);
    PROTOCOL_CHECK(automatically_captured.input_items[0].at("id") == "msg_resp_protocol_input_0");
    PROTOCOL_CHECK(automatically_captured.input_items[1].at("id") == "msg_resp_protocol_input_1");
    PROTOCOL_CHECK(automatically_captured.input_items[2].at("id") == "msg_resp_protocol_input_2");

    common_json rendered = render_response(state);
    PROTOCOL_CHECK(rendered.at("object").get<std::string>() == "response");
    PROTOCOL_CHECK(rendered.at("output_text").get<std::string>() == "hello");
    PROTOCOL_CHECK(rendered.at("output")[1].at("call_id").get<std::string>() == "call_protocol");
    PROTOCOL_CHECK(rendered.at("temperature").get<double>() == 0.25);
    PROTOCOL_CHECK(rendered.at("usage").at("total_tokens").get<unsigned long long>() == 10);
    PROTOCOL_CHECK(rendered.at("usage").at("input_tokens_details").at("cache_write_tokens").get<int>() == 5);
    PROTOCOL_CHECK(rendered.at("usage").at("output_tokens_details").at("future_detail").get<int>() == 7);
    PROTOCOL_CHECK(rendered.at("future_wire_field").at("preserved").get<bool>());

    common_json terminal = render_terminal_event(state, 9);
    PROTOCOL_CHECK(terminal.at("type").get<std::string>() == "response.completed");
    PROTOCOL_CHECK(terminal.at("sequence_number").get<unsigned long long>() == 9);
    PROTOCOL_CHECK(terminal.at("response").at("id").get<std::string>() == "resp_protocol");
}

void test_resource_service() {
    common_json request = make_request();
    common_json inputs  = capture_input_items(
        request, [](std::size_t index, const common_json &) { return item_id("msg_input_" + std::to_string(index)); });
    response_state state = capture_response_state(make_wire_response(), request, inputs);

    in_memory_response_store store;
    PROTOCOL_CHECK(store.create(state) == store_write_result::stored);
    response_resource_service service(store);

    resource_result retrieved = service.retrieve(response_id("resp_protocol"));
    PROTOCOL_CHECK(retrieved.ok());
    PROTOCOL_CHECK(retrieved.body.at("id").get<std::string>() == "resp_protocol");

    input_item_page_options page;
    page.limit             = 2;
    resource_result listed = service.list_input_items(response_id("resp_protocol"), page);
    PROTOCOL_CHECK(listed.ok());
    PROTOCOL_CHECK(listed.body.at("data").size() == 2);
    PROTOCOL_CHECK(listed.body.at("first_id").get<std::string>() == "msg_input_2");
    PROTOCOL_CHECK(listed.body.at("last_id").get<std::string>() == "msg_input_1");
    PROTOCOL_CHECK(listed.body.at("has_more").get<bool>());

    page.after = item_id("msg_input_1");
    listed     = service.list_input_items(response_id("resp_protocol"), page);
    PROTOCOL_CHECK(listed.ok());
    PROTOCOL_CHECK(listed.body.at("data").size() == 1);
    PROTOCOL_CHECK(listed.body.at("first_id").get<std::string>() == "msg_input_0");
    PROTOCOL_CHECK(!listed.body.at("has_more").get<bool>());

    resource_result event =
        service.completed_payload(response_id("resp_protocol"), completed_payload_kind::sse_event, 12);
    PROTOCOL_CHECK(event.ok());
    PROTOCOL_CHECK(event.body.at("type").get<std::string>() == "response.completed");

    page.limit              = 101;
    resource_result invalid = service.list_input_items(response_id("resp_protocol"), page);
    PROTOCOL_CHECK(invalid.kind == resource_result_kind::invalid_request);
    PROTOCOL_CHECK(invalid.body.at("error").at("param").get<std::string>() == "limit");

    resource_result deleted = service.erase(response_id("resp_protocol"));
    PROTOCOL_CHECK(deleted.ok());
    PROTOCOL_CHECK(deleted.body.at("deleted").get<bool>());
    PROTOCOL_CHECK(deleted.body.at("object").get<std::string>() == "response");

    resource_result missing = service.retrieve(response_id("resp_protocol"));
    PROTOCOL_CHECK(missing.kind == resource_result_kind::not_found);
    PROTOCOL_CHECK(missing.body.at("error").at("code").get<std::string>() == "response_not_found");
}

void test_errors_and_validation() {
    common_json error =
        render_error("bad input", "invalid_request_error", std::string("input"), std::string("invalid_value"));
    PROTOCOL_CHECK(error.at("error").at("message").get<std::string>() == "bad input");
    PROTOCOL_CHECK(error.at("error").at("param").get<std::string>() == "input");

    common_json failed_wire     = make_wire_response();
    failed_wire["status"]       = "failed";
    failed_wire["completed_at"] = nullptr;
    failed_wire["usage"]        = nullptr;
    failed_wire["error"]        = {
        { "code",                "generation_failed" },
        { "message",             "fixture failure"   },
        { "future_error_detail", 9                   },
    };
    response_state failed_state    = capture_response_state(failed_wire, make_request());
    common_json    failed_rendered = render_response(failed_state);
    PROTOCOL_CHECK(failed_rendered.at("usage").is_null());
    PROTOCOL_CHECK(failed_rendered.at("error").at("future_error_detail").get<int>() == 9);

    bool threw = false;
    try {
        response_state state;
        state.id     = response_id("resp_live");
        state.status = response_status::in_progress;
        (void) render_terminal_event(state, 0);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    PROTOCOL_CHECK(threw);

    input_item_page_options invalid;
    invalid.limit = 101;
    threw         = false;
    try {
        (void) render_input_items_page(common_json::array(), invalid);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    PROTOCOL_CHECK(threw);
}

}  // namespace

// This function intentionally has external linkage; test-foundation.cpp owns
// main() so both protocol and foundation checks share one executable.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_protocol() {
    test_capture_and_render();
    test_resource_service();
    test_errors_and_validation();
    return failures;
}
