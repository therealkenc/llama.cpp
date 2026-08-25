#include "json.h"
#include "server-chat.h"

#include <exception>
#include <iostream>
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
    CHECK(system_message.at("content")[1].at("image_url").at("url").get<std::string>() ==
          "data:image/png;base64,AA==");

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

} // namespace

int main() try {
    test_typed_instructions();
    test_structured_output_lowering();
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
