#include "chat.h"
#include "common.h"
#include "input-lowering.h"
#include "json.h"
#include "server-common.h"
#include "server-generation-internal.h"
#include "server-generation.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
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
    common_chat_session    session;
    bool                    rejected = false;
    try {
        static_cast<void>(server_generation_params_parse(nullptr, typed, options, files, session));
    } catch (const std::invalid_argument & error) {
        rejected = std::string(error.what()).find("image input is not supported") != std::string::npos;
    }
    CHECK(rejected);
    CHECK(files.empty());
}

void test_typed_server_adapter_preserves_chat_session() {
    const auto source_dir = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto vocab_path = source_dir / "models/ggml-vocab-qwen35.gguf";
    std::ifstream template_file(source_dir / "models/templates/Qwen3.5-4B.jinja", std::ios::binary);
    if (!template_file) {
        throw std::runtime_error("failed to open Qwen3.5 chat template fixture");
    }
    const std::string template_text{ std::istreambuf_iterator<char>(template_file), std::istreambuf_iterator<char>() };

    auto model_params = llama_model_default_params();
    model_params.vocab_only = true;
    const std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
        llama_model_load_from_file(vocab_path.string().c_str(), model_params), llama_model_free);
    if (!model) {
        throw std::runtime_error("failed to load Qwen3.5 vocabulary fixture");
    }
    const auto * vocab = llama_model_get_vocab(model.get());

    server_chat_params options{};
    options.use_jinja        = true;
    options.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    options.tmpls = common_chat_templates_ptr(common_chat_templates_init(nullptr, template_text));
    const server_generation_input typed = lower_responses_generation_input(common_json::parse(R"({
        "input": "Look up the weather.",
        "tools": [{
            "type": "function",
            "name": "get_weather",
            "parameters": {
                "type": "object",
                "properties": {"city": {"type": "string"}},
                "required": ["city"]
            }
        }]
    })"));
    std::vector<raw_buffer> files;
    common_chat_session session;
    const json data = server_generation_params_parse(vocab, typed, options, files, session);
    CHECK(session.has_template());
    CHECK(data.at("prompt") == session.prompt());
    CHECK(!session.grammar().empty());
    CHECK(!data.contains("grammar"));
    CHECK(files.empty());
    CHECK(!session.message_delimiters().delimiters.empty());
    for (const auto & delimiter : session.message_delimiters().delimiters) {
        CHECK(!delimiter.tokens.empty());
    }

    common_params_sampling sampling;
    session.apply_sampling(sampling);
    CHECK(sampling.grammar.grammar == session.grammar());
    CHECK(sampling.generation_prompt == session.generation_prompt());

    const auto & message = session.finish(common_chat_input_tokenize(vocab,
        "Checking.</think>\n\n<tool_call>\n<function=get_weather>\n"
        "<parameter=city>\nTokyo\n</parameter>\n</function>\n</tool_call>"));
    CHECK(message.reasoning_content == "Checking.");
    CHECK(message.tool_calls.size() == 1U);
    if (!message.tool_calls.empty()) {
        CHECK(message.tool_calls.front().name == "get_weather");
        CHECK(json::parse(message.tool_calls.front().arguments) == json({{ "city", "Tokyo" }}));
    }
}

}  // namespace

int main() try {
    test_typed_instructions();
    test_structured_output_lowering();
    test_reasoning_effort_lowering();
    test_local_shell_protocol_identity_cannot_be_renamed();
    test_namespace_tools_lower_to_unique_functions_and_replay();
    test_typed_server_adapter_rejects_unsupported_media();
    test_typed_server_adapter_preserves_chat_session();
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
