#include "json.h"
#include "server-chat.h"

#include <exception>
#include <iostream>
#include <set>
#include <string>

namespace {

int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

void test_typed_instructions() {
    const common_json request = common_json::parse(R"({
        "input": "Describe it briefly.",
        "instructions": [
            {
                "type": "message",
                "role": "developer",
                "content": [
                    {"type": "input_text", "text": "Inspect the supplied image."},
                    {"type": "input_image", "image_url": "data:image/png;base64,AA=="}
                ]
            }
        ],
        "model": "test-model"
    })");

    const common_json result = server_chat_convert_responses_to_chatcmpl(request);
    CHECK(result.at("messages").size() == 2);

    const auto & system_message = result.at("messages")[0];
    CHECK(system_message.at("role").get<std::string>() == "system");
    CHECK(system_message.at("content")[0].at("type").get<std::string>() == "text");
    CHECK(system_message.at("content")[0].at("text").get<std::string>() == "Inspect the supplied image.");
    CHECK(system_message.at("content")[1].at("type").get<std::string>() == "image_url");
    CHECK(system_message.at("content")[1].at("image_url").at("url").get<std::string>() == "data:image/png;base64,AA==");

    const auto & user_message = result.at("messages")[1];
    CHECK(user_message.at("role").get<std::string>() == "user");
    CHECK(user_message.at("content")[0].at("text").get<std::string>() == "Describe it briefly.");
}

void test_structured_output_lowering() {
    const common_json request = common_json::parse(R"({
        "input": "Return an object",
        "model": "test-model",
        "text": {
            "format": {
                "type": "json_schema",
                "name": "answer",
                "strict": true,
                "schema": {
                    "type": "object",
                    "properties": {"answer": {"type": "string"}},
                    "required": ["answer"],
                    "additionalProperties": false
                }
            }
        }
    })");

    const common_json result = server_chat_convert_responses_to_chatcmpl(request);
    CHECK(!result.contains("text"));
    CHECK(result.at("response_format").at("type").get<std::string>() == "json_schema");

    const auto & schema = result.at("response_format").at("json_schema");
    CHECK(schema.at("name").get<std::string>() == "answer");
    CHECK(schema.at("strict").get<bool>());
    CHECK(schema.at("schema").at("type").get<std::string>() == "object");
}

void test_reasoning_effort_lowering() {
    const common_json request = common_json::parse(R"({
        "input": "Think briefly.",
        "model": "qwen3.8-27b-local",
        "reasoning": {"effort": "low"}
    })");

    const common_json result = server_chat_convert_responses_to_chatcmpl(request);
    CHECK(result.at("reasoning_effort").get<std::string>() == "low");
    CHECK(!result.contains("reasoning"));
}

void test_local_shell_protocol_identity_cannot_be_renamed() {
    const common_json request = common_json::parse(R"({
        "input": "Inspect the repository.",
        "tools": [
            {
                "type": "local_shell",
                "name": "bypass_policy_identity"
            }
        ]
    })");

    const common_json lowered = server_chat_convert_responses_to_chatcmpl(request);
    CHECK(lowered.at("tools").at(0).at("function").at("name") == "local_shell");
    CHECK(lowered.at("__responses_tool_metadata").at("local_shell").at("type") == "local_shell");
}

void test_namespace_tools_lower_to_unique_functions_and_replay() {
    const common_json request = common_json::parse(R"({
        "input": "Use a connector only if needed.",
        "tools": [
            {
                "type": "namespace",
                "name": "mcp__calendar",
                "description": "Calendar connector tools.",
                "tools": [
                    {
                        "type": "function",
                        "name": "lookup",
                        "description": "Look up an event.",
                        "strict": false,
                        "parameters": {"type": "object", "properties": {}}
                    }
                ]
            },
            {
                "type": "namespace",
                "name": "mcp__mail",
                "description": "Mail connector tools.",
                "tools": [
                    {
                        "type": "function",
                        "name": "lookup",
                        "description": "Look up a message.",
                        "strict": false,
                        "parameters": {"type": "object", "properties": {}}
                    }
                ]
            }
        ]
    })");

    const common_json lowered = server_chat_convert_responses_to_chatcmpl(request);
    CHECK(lowered.at("tools").size() == 2);
    CHECK(lowered.at("__responses_tool_metadata").size() == 2);

    std::set<std::string> chat_names;
    std::string           calendar_chat_name;
    for (const common_json & tool : lowered.at("tools")) {
        const std::string chat_name = tool.at("function").at("name").get<std::string>();
        CHECK(chat_name.size() <= 64);
        CHECK(chat_names.insert(chat_name).second);
        const common_json & metadata = lowered.at("__responses_tool_metadata").at(chat_name);
        CHECK(metadata.at("name") == "lookup");
        CHECK(metadata.at("type") == "function");
        if (metadata.at("namespace") == "mcp__calendar") {
            calendar_chat_name = chat_name;
        }
    }
    CHECK(!calendar_chat_name.empty());

    const common_json replay         = common_json::parse(R"({
        "input": [
            {
                "type": "function_call",
                "namespace": "mcp__calendar",
                "name": "lookup",
                "call_id": "call_fixture",
                "arguments": "{}"
            }
        ]
    })");
    const common_json replay_lowered = server_chat_convert_responses_to_chatcmpl(replay);
    CHECK(replay_lowered.at("messages").at(0).at("tool_calls").at(0).at("function").at("name") == calendar_chat_name);
}

}  // namespace

int main() try {
    test_typed_instructions();
    test_structured_output_lowering();
    test_reasoning_effort_lowering();
    test_local_shell_protocol_identity_cannot_be_renamed();
    test_namespace_tools_lower_to_unique_functions_and_replay();
    if (failures != 0) {
        std::cerr << failures << " server-chat Responses checks failed\n";
        return 1;
    }
    std::cout << "llama-responses server-chat checks passed\n";
    return 0;
} catch (const std::exception & error) {
    std::cerr << "llama-responses server-chat checks threw: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "llama-responses server-chat checks threw an unknown exception\n";
    return 1;
}
