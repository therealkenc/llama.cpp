#include "input-lowering.h"
#include "json.h"
#include "server-generation.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

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

void test_mixed_multimodal_tool_outputs_preserve_order_and_call_ids() {
    const common_json request = {
        { "model",               "fixture-model"             },
        { "instructions",        "Inspect every attachment." },
        { "input",
         common_json::array({
              {
                  { "type", "function_call" },
                  { "id", "fc_public_item" },
                  { "call_id", "call_function" },
                  { "name", "inspect" },
                  { "arguments", R"({"path":"a"})" },
              },
              {
                  { "type", "function_call_output" },
                  { "call_id", "call_function" },
                  { "output", common_json::array({
                                  { { "type", "input_text" }, { "text", "before" } },
                                  { { "type", "input_image" }, { "image_url", "data:image/png;base64,QUFB" } },
                                  { { "type", "input_text" }, { "text", "between" } },
                                  { { "type", "image" }, { "mimeType", "image/jpeg" }, { "data", "QkJC" } },
                              }) },
              },
              {
                  { "type", "custom_tool_call" },
                  { "id", "ctc_public_item" },
                  { "call_id", "call_custom" },
                  { "name", "browser_result" },
                  { "input", "capture" },
              },
              {
                  { "type", "custom_tool_call_output" },
                  { "call_id", "call_custom" },
                  { "output", common_json::array({
                                  { { "type", "input_image" }, { "image_url", "https://example.invalid/c.png" } },
                                  { { "type", "input_text" }, { "text", "after" } },
                              }) },
              },
          })                                                 },
        { "tools",               common_json::array({
                       {
                           { "type", "function" },
                           { "name", "inspect" },
                           { "parameters",
                             {
                                 { "type", "object" },
                                 { "properties", { { "path", { { "type", "string" } } } } },
                                 { "required", common_json::array({ "path" }) },
                             } },
                       },
                       {
                           { "type", "custom" },
                           { "name", "browser_result" },
                       },
                   })                      },
        { "tool_choice",         "auto"                      },
        { "parallel_tool_calls", true                        },
        { "max_output_tokens",   64                          },
        { "reasoning",           { { "effort", "low" } }     },
    };

    const server_generation_input lowered = lower_responses_generation_input(request);
    CHECK(lowered.chat.messages.size() == 5U);
    CHECK(lowered.chat.messages.at(0).role == "system");
    CHECK(lowered.chat.messages.at(1).tool_calls.size() == 1U);
    CHECK(lowered.chat.messages.at(1).tool_calls.at(0).id == "call_function");
    CHECK(lowered.chat.messages.at(1).tool_calls.at(0).id != "fc_public_item");
    CHECK(lowered.chat.messages.at(2).role == "tool");
    CHECK(lowered.chat.messages.at(2).tool_call_id == "call_function");
    CHECK(lowered.chat.messages.at(2).content_parts.size() == 4U);
    CHECK(lowered.chat.messages.at(2).content_parts.at(0).text == "before");
    CHECK(lowered.chat.messages.at(2).content_parts.at(1).type == "media_marker");
    CHECK(lowered.chat.messages.at(2).content_parts.at(2).text == "between");
    CHECK(lowered.chat.messages.at(2).content_parts.at(3).type == "media_marker");
    CHECK(lowered.chat.messages.at(3).tool_calls.at(0).id == "call_custom");
    CHECK(lowered.chat.messages.at(4).tool_call_id == "call_custom");
    CHECK(lowered.chat.messages.at(4).content_parts.at(0).type == "media_marker");
    CHECK(lowered.chat.messages.at(4).content_parts.at(1).text == "after");

    CHECK(lowered.media.size() == 3U);
    CHECK(lowered.media.at(0).message_index == 2U);
    CHECK(lowered.media.at(0).content_part_index == 1U);
    CHECK(lowered.media.at(0).source == "data:image/png;base64,QUFB");
    CHECK(lowered.media.at(1).message_index == 2U);
    CHECK(lowered.media.at(1).content_part_index == 3U);
    CHECK(lowered.media.at(1).source == "data:image/jpeg;base64,QkJC");
    CHECK(lowered.media.at(2).message_index == 4U);
    CHECK(lowered.media.at(2).content_part_index == 0U);
    CHECK(lowered.media.at(2).source == "https://example.invalid/c.png");

    CHECK(lowered.chat.tools.size() == 2U);
    CHECK(lowered.tool_metadata.at("inspect").at("type") == "function");
    CHECK(lowered.tool_metadata.at("browser_result").at("type") == "custom");
    CHECK(lowered.parallel_tool_calls && *lowered.parallel_tool_calls);
    CHECK(lowered.inference_parameters.at("max_tokens") == 64);
    CHECK(lowered.inference_parameters.at("reasoning_effort") == "low");
}

void test_image_file_and_computer_screenshot_remain_media() {
    const common_json request = {
        { "input", common_json::array({
                       {
                           { "type", "message" },
                           { "role", "user" },
                           { "content", common_json::array({
                                            {
                                                { "type", "input_file" },
                                                { "filename", "chart.png" },
                                                { "file_data", "data:image/png;base64,Q0ND" },
                                            },
                                        }) },
                       },
                       {
                           { "type", "computer_call_output" },
                           { "call_id", "call_browser" },
                           { "output",
                             {
                                 { "type", "computer_screenshot" },
                                 { "image_url", "data:image/png;base64,RERE" },
                             } },
                       },
                   }) },
    };

    const server_generation_input lowered = lower_responses_generation_input(request);
    CHECK(lowered.chat.messages.size() == 2U);
    CHECK(lowered.chat.messages.at(0).content_parts.size() == 2U);
    CHECK(lowered.chat.messages.at(0).content_parts.at(0).text == "[file: chart.png]");
    CHECK(lowered.chat.messages.at(0).content_parts.at(1).type == "media_marker");
    CHECK(lowered.chat.messages.at(1).role == "tool");
    CHECK(lowered.chat.messages.at(1).tool_call_id == "call_browser");
    CHECK(lowered.chat.messages.at(1).content_parts.at(0).type == "media_marker");
    CHECK(lowered.media.size() == 2U);
    CHECK(lowered.media.at(0).source == "data:image/png;base64,Q0ND");
    CHECK(lowered.media.at(1).source == "data:image/png;base64,RERE");
}

void test_text_files_accept_openai_base64_and_legacy_decoded_content() {
    const common_json request = {
        { "input", common_json::array({
                       {
                           { "type", "message" },
                           { "role", "user" },
                           { "content", common_json::array({
                                            {
                                                { "type", "input_file" },
                                                { "filename", "encoded.txt" },
                                                { "file_data", "aGVsbG8=" },
                                            },
                                            {
                                                { "type", "input_file" },
                                                { "filename", "decoded.txt" },
                                                { "file_data", "hello world" },
                                            },
                                        }) },
                       },
                   }) },
    };

    const server_generation_input lowered = lower_responses_generation_input(request);
    CHECK(lowered.chat.messages.at(0).content_parts.at(0).text == "[file: encoded.txt]\nhello");
    CHECK(lowered.chat.messages.at(0).content_parts.at(1).text == "[file: decoded.txt]\nhello world");
    CHECK(lowered.media.empty());
}

void test_unresolved_reference_and_file_provider_are_explicit_errors() {
    for (const common_json & input : {
             common_json{ { "type", "item_reference" }, { "id", "msg_missing" } },
             common_json{
                         { "type", "message" },
                         { "role", "user" },
                         { "content", common_json::array({
                                  { { "type", "input_file" }, { "file_id", "file_missing" } },
                              }) },
                         },
    }) {
        bool threw = false;
        try {
            const server_generation_input unused = lower_responses_generation_input({
                { "input", common_json::array({ input }) }
            });
            static_cast<void>(unused);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        CHECK(threw);
    }
}

void test_malformed_content_recovers_without_discarding_valid_media() {
    const common_json request = {
        { "input", common_json::array({
                       {
                           { "type", "message" },
                           { "role", "user" },
                           { "content", common_json::array({
                                            { { "type", "input_text" } },
                                            { { "type", "input_audio" }, { "data", "future-shape" } },
                                            { { "type", "input_file" }, { "filename", "report.pdf" } },
                                            {
                                                { "type", "input_image" },
                                                { "image_url", "data:image/png;base64,QUFB" },
                                            },
                                        }) },
                       },
                   }) },
    };

    const server_generation_input lowered = lower_responses_generation_input(request);
    CHECK(lowered.chat.messages.size() == 1U);
    CHECK(lowered.chat.messages.at(0).content_parts.size() == 4U);
    CHECK(lowered.chat.messages.at(0).content_parts.at(0).text.find("responses recovery") != std::string::npos);
    CHECK(lowered.chat.messages.at(0).content_parts.at(1).text.find("responses recovery") != std::string::npos);
    CHECK(lowered.chat.messages.at(0).content_parts.at(2).text == "[file: report.pdf data unavailable]");
    CHECK(lowered.chat.messages.at(0).content_parts.at(3).type == "media_marker");
    CHECK(lowered.media.size() == 1U);
}

void test_string_instructions_merge_with_developer_items_as_typed_content() {
    const common_json request = {
        { "instructions", "Base Codex instructions." },
        { "input",        common_json::array({
                       {
                           { "type", "message" },
                           { "role", "developer" },
                           { "content", "Repository instructions." },
                       },
                       {
                           { "type", "message" },
                           { "role", "user" },
                           { "content", "Do the work." },
                       },
                   })              },
    };

    const server_generation_input lowered = lower_responses_generation_input(request);
    CHECK(lowered.chat.messages.size() == 2U);
    CHECK(lowered.chat.messages.at(0).role == "system");
    CHECK(lowered.chat.messages.at(0).content.empty());
    CHECK(lowered.chat.messages.at(0).content_parts.size() == 2U);
    CHECK(lowered.chat.messages.at(0).content_parts.at(0).text == "Base Codex instructions.");
    CHECK(lowered.chat.messages.at(0).content_parts.at(1).text == "Repository instructions.");
    CHECK(lowered.chat.messages.at(1).role == "user");
    CHECK(lowered.chat.messages.at(1).content.empty());
    CHECK(lowered.chat.messages.at(1).content_parts.at(0).text == "Do the work.");
}

}  // namespace

int main() try {
    test_mixed_multimodal_tool_outputs_preserve_order_and_call_ids();
    test_image_file_and_computer_screenshot_remain_media();
    test_text_files_accept_openai_base64_and_legacy_decoded_content();
    test_unresolved_reference_and_file_provider_are_explicit_errors();
    test_malformed_content_recovers_without_discarding_valid_media();
    test_string_instructions_merge_with_developer_items_as_typed_content();
    if (failures != 0) {
        std::cerr << failures << " input lowering checks failed\n";
        return 1;
    }
    std::cout << "llama-responses input lowering checks passed\n";
    return 0;
} catch (const std::exception & error) {
    std::cerr << "input lowering checks threw: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "input lowering checks threw an unknown exception\n";
    return 1;
}
