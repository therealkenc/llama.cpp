#include "chat.h"
#include "common.h"
#include "input-lowering.h"
#include "json.h"
#include "server-common.h"
#include "server-generation-internal.h"
#include "server-generation.h"

#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
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

    const server_generation_input typed = lower_responses_generation_input(request);
    CHECK(typed.chat.messages.size() == 2U);
    CHECK(typed.chat.messages.at(0).role == "system");
    CHECK(typed.chat.messages.at(0).content_parts.at(0).text == "Inspect the supplied image.");
    CHECK(typed.chat.messages.at(0).content_parts.at(1).type == "media_marker");
    CHECK(typed.chat.messages.at(1).role == "user");
    CHECK(typed.chat.messages.at(1).content.empty());
    CHECK(typed.chat.messages.at(1).content_parts.at(0).text == "Describe it briefly.");
    CHECK(typed.media.size() == 1U);
    CHECK(typed.media.at(0).message_index == 0U);
    CHECK(typed.media.at(0).content_part_index == 1U);
    CHECK(typed.media.at(0).source == "data:image/png;base64,AA==");
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

    const server_generation_input typed = lower_responses_generation_input(request);
    CHECK(common_json::parse(typed.chat.json_schema) == request.at("text").at("format").at("schema"));
}

void test_reasoning_effort_lowering() {
    const common_json request = common_json::parse(R"({
        "input": "Think briefly.",
        "model": "qwen3.8-27b-local",
        "reasoning": {"effort": "low"}
    })");

    const server_generation_input typed = lower_responses_generation_input(request);
    CHECK(typed.inference_parameters.at("reasoning_effort") == "low");
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

    const server_generation_input typed = lower_responses_generation_input(request);
    CHECK(typed.chat.tools.at(0).name == "local_shell");
    CHECK(typed.tool_metadata.at("local_shell").at("type") == "local_shell");
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

    const common_json             replay = common_json::parse(R"({
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
    const server_generation_input typed  = lower_responses_generation_input(request);
    CHECK(typed.chat.tools.size() == 2U);
    CHECK(typed.tool_metadata.size() == 2U);
    std::set<std::string> typed_names;
    std::string           calendar_chat_name;
    for (const common_chat_tool & tool : typed.chat.tools) {
        CHECK(tool.name.size() <= 64U);
        CHECK(typed_names.insert(tool.name).second);
        CHECK(typed.tool_metadata.at(tool.name).at("name") == "lookup");
        if (typed.tool_metadata.at(tool.name).at("namespace") == "mcp__calendar") {
            calendar_chat_name = tool.name;
        }
    }
    CHECK(!calendar_chat_name.empty());

    const server_generation_input typed_replay = lower_responses_generation_input(replay);
    CHECK(typed_replay.chat.messages.at(0).tool_calls.at(0).name == calendar_chat_name);
}

void test_typed_server_adapter_rejects_unsupported_media() {
    const server_generation_input typed = lower_responses_generation_input(common_json::parse(R"({
        "input": [{
            "type": "message",
            "role": "user",
            "content": [{"type": "input_image", "image_url": "data:image/png;base64,AA=="}]
        }]
    })"));
    server_chat_params            options;
    options.use_jinja         = false;
    options.prefill_assistant = false;
    options.reasoning_format  = COMMON_REASONING_FORMAT_NONE;
    options.allow_image       = false;
    options.allow_audio       = false;
    options.allow_video       = false;
    std::vector<raw_buffer> files;
    bool                    rejected = false;
    try {
        static_cast<void>(server_generation_params_parse(typed, options, files));
    } catch (const std::invalid_argument & error) {
        rejected = std::string(error.what()).find("image input is not supported") != std::string::npos;
    }
    CHECK(rejected);
    CHECK(files.empty());
}

}  // namespace

int main() try {
    test_typed_instructions();
    test_structured_output_lowering();
    test_reasoning_effort_lowering();
    test_local_shell_protocol_identity_cannot_be_renamed();
    test_namespace_tools_lower_to_unique_functions_and_replay();
    test_typed_server_adapter_rejects_unsupported_media();
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
