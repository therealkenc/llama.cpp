#include "server-integration.h"

#include "codex-models.h"
#include "generation.h"
#include "hosted-tools.h"
#include "json.h"
#include "log.h"
#include "protocol-codec.h"
#include "response-service.h"
#include "response-store.h"
#include "response-types.h"
#include "server-generation-adapter.h"
#include "server-http.h"
#include "server-responses.h"
#include "server-route-extensions.h"
#include "sqlite-response-store.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace llama_responses {
namespace {

server_http_res_ptr json_response(const common_json & body, int status = 200) {
    auto response    = std::make_unique<server_http_res>();
    response->status = status;
    response->data   = body.dump();
    return response;
}

server_http_res_ptr api_error(int                                status,
                              const std::string &                message,
                              const std::string &                code,
                              const std::optional<std::string> & param = std::nullopt) {
    std::string type = "invalid_request_error";
    if (status >= 500 && code != "not_supported") {
        type = "server_error";
    }
    const std::optional<std::string> rendered_code = code.empty() ? std::nullopt : std::optional<std::string>(code);
    return json_response(render_error(message, type, param, rendered_code), status);
}

int status_for(resource_result_kind kind) {
    switch (kind) {
        case resource_result_kind::ok:
            return 200;
        case resource_result_kind::not_found:
            return 404;
        case resource_result_kind::invalid_request:
            return 400;
        case resource_result_kind::conflict:
            return 409;
    }
    return 500;
}

bool request_store_enabled(const common_json & request) {
    if (!request.is_object() || !request.contains("store") || request.at("store").is_null()) {
        return true;
    }
    return request.at("store").is_boolean() && request.at("store").get<bool>();
}

bool native_generation_supported(const common_json & request) {
    // These llama-server telemetry extensions carry payloads which the neutral
    // generation vocabulary intentionally does not expose yet. Keep their
    // existing renderer as an explicit oracle rather than silently dropping
    // requested data from the native projection.
    static constexpr std::array<const char *, 2> telemetry_extensions = { "return_progress", "timings_per_token" };
    return std::all_of(telemetry_extensions.begin(), telemetry_extensions.end(), [&](const char * key) {
        return !request.contains(key) || request.at(key).is_null() ||
               (request.at(key).is_boolean() && !request.at(key).get<bool>());
    });
}

std::string response_id_param(const server_http_req & request) {
    return request.get_param("response_id");
}

common_json parse_json_body(const server_http_req & request) {
    common_json body = common_json::parse(request.body);
    if (!body.is_object()) {
        throw std::invalid_argument("Responses request body must be a JSON object");
    }
    return body;
}

std::string strip_ascii_space(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::size_t utf8_character_count(const std::string & value) {
    return static_cast<std::size_t>(
        std::count_if(value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xc0U) != 0x80U; }));
}

bool json_integer_is_negative(const common_json & value) {
    // common_json intentionally hides the backing library's signed/unsigned
    // tag. JSON's integer spelling is sufficient here and avoids narrowing a
    // valid uint64_t through int64_t merely to validate a range.
    const std::string encoded = value.dump();
    return !encoded.empty() && encoded.front() == '-';
}

std::string random_id_suffix() {
    static constexpr char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device    source;
    std::uniform_int_distribution<std::size_t> distribution(0, sizeof(alphabet) - 2);
    std::string                                result(32, ' ');
    for (char & character : result) {
        character = alphabet[distribution(source)];
    }
    return result;
}

class unsupported_request_field final : public std::runtime_error {
  public:
    unsupported_request_field(std::string field, const std::string & message) :
        std::runtime_error(message),
        field(std::move(field)) {}

    const std::string field;
};

class invalid_request_field final : public std::runtime_error {
  public:
    invalid_request_field(std::string field, std::string code, const std::string & message) :
        std::runtime_error(message),
        field(std::move(field)),
        code(std::move(code)) {}

    const std::string field;
    const std::string code;
};

void validate_top_level_fields(const common_json & request) {
    static const std::set<std::string> known_fields = {
        "background",
        "client_metadata",
        "context_management",
        "conversation",
        "frequency_penalty",
        "include",
        "input",
        "instructions",
        "max_output_tokens",
        "max_tool_calls",
        "metadata",
        "model",
        "moderation",
        "parallel_tool_calls",
        "personality",
        "presence_penalty",
        "previous_response_id",
        "prompt",
        "prompt_cache_key",
        "prompt_cache_options",
        "prompt_cache_retention",
        "reasoning",
        // Existing llama-server telemetry extensions remain available on the
        // Responses aliases. They are intentionally outside the OpenAI schema.
        "return_progress",
        "safety_identifier",
        "service_tier",
        "store",
        "stream",
        "stream_options",
        "temperature",
        "text",
        "timings_per_token",
        "tool_choice",
        "tools",
        "top_logprobs",
        "top_p",
        "truncation",
        "user",
        "web_search",
    };
    for (const auto & entry : request.items()) {
        if (known_fields.find(entry.key()) == known_fields.end()) {
            throw invalid_request_field(entry.key(), "unknown_parameter", "Unknown parameter: '" + entry.key() + "'.");
        }
    }
}

void normalize_nullable_defaults(common_json & request) {
    const auto set_default = [&request](const char * key, common_json value) {
        if (request.contains(key) && request.at(key).is_null()) {
            request[key] = std::move(value);
        }
    };
    set_default("store", true);
    set_default("background", false);
    set_default("stream", false);
    set_default("metadata", common_json::object());
    set_default("text", common_json{
                            { "format", { { "type", "text" } } }
    });
    set_default("truncation", "disabled");
    set_default("service_tier", "default");
    set_default("top_logprobs", 0);
}

void validate_optional_identifier(const common_json & request, const char * key, std::size_t max_length) {
    if (!request.contains(key) || request.at(key).is_null()) {
        return;
    }
    if (!request.at(key).is_string()) {
        throw invalid_request_field(key, "invalid_type",
                                    std::string("Invalid type for '") + key + "': expected a string or null.");
    }
    if (utf8_character_count(request.at(key).get<std::string>()) > max_length) {
        throw invalid_request_field(key, "string_above_max_length",
                                    std::string("Invalid '") + key +
                                        "': string too long. Expected a string with maximum length " +
                                        std::to_string(max_length) + ".");
    }
}

void validate_metadata(const common_json & request) {
    if (!request.contains("metadata") || request.at("metadata").is_null()) {
        return;
    }
    const common_json & metadata = request.at("metadata");
    if (!metadata.is_object()) {
        throw invalid_request_field("metadata", "invalid_type",
                                    "Invalid type for 'metadata': expected an object or null.");
    }
    if (metadata.size() > 16) {
        throw invalid_request_field(
            "metadata", "object_above_max_properties",
            "Invalid 'metadata': too many properties. Expected an object with at most 16 properties.");
    }
    for (const auto & entry : metadata.items()) {
        const std::string field = "metadata." + entry.key();
        if (utf8_character_count(entry.key()) > 64) {
            throw invalid_request_field(
                field, "property_name_above_max_length",
                "Invalid property name in 'metadata' is too long. Expected a maximum length of 64.");
        }
        if (!entry.value().is_string()) {
            throw invalid_request_field(field, "invalid_type", "Invalid type for '" + field + "': expected a string.");
        }
        if (utf8_character_count(entry.value().get<std::string>()) > 512) {
            throw invalid_request_field(field, "string_above_max_length",
                                        "Invalid '" + field + "': string too long. Expected a maximum length of 512.");
        }
    }
}

void validate_client_metadata(const common_json & request) {
    if (!request.contains("client_metadata") || request.at("client_metadata").is_null()) {
        return;
    }
    const common_json & metadata = request.at("client_metadata");
    if (!metadata.is_object()) {
        throw invalid_request_field("client_metadata", "invalid_type",
                                    "Invalid type for 'client_metadata': expected an object or null.");
    }
    for (const auto & entry : metadata.items()) {
        if (!entry.value().is_string()) {
            const std::string field = "client_metadata." + entry.key();
            throw invalid_request_field(field, "invalid_type", "Invalid type for '" + field + "': expected a string.");
        }
    }
}

bool valid_tool_name(const std::string & name, std::size_t max_length) {
    return !name.empty() && name.size() <= max_length &&
           std::all_of(name.begin(), name.end(), [](unsigned char character) {
               return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '_' || character == '-';
           });
}

std::string require_tool_name(const common_json & value, const std::string & field, std::size_t max_length = 64U) {
    if (!value.contains("name")) {
        throw invalid_request_field(field, "missing_required_parameter",
                                    "Missing required parameter: '" + field + "'.");
    }
    if (!value.at("name").is_string()) {
        throw invalid_request_field(field, "invalid_type", "Invalid type for '" + field + "': expected a string.");
    }
    const std::string name = value.at("name").get<std::string>();
    if (!valid_tool_name(name, max_length)) {
        throw invalid_request_field(field, "invalid_value",
                                    "Invalid value for '" + field + "': expected 1-" + std::to_string(max_length) +
                                        " letters, numbers, underscores, or hyphens.");
    }
    return name;
}

void validate_optional_tool_field(const common_json & tool,
                                  const char *        name,
                                  const std::string & prefix,
                                  bool (common_json::*type_predicate)() const,
                                  const char * expected) {
    if (!tool.contains(name) || tool.at(name).is_null()) {
        return;
    }
    if (!(tool.at(name).*type_predicate)()) {
        const std::string field = prefix + "." + name;
        throw invalid_request_field(field, "invalid_type",
                                    "Invalid type for '" + field + "': expected " + expected + ".");
    }
}

void validate_custom_tool_format(const common_json & tool, const std::string & prefix) {
    if (!tool.contains("format") || tool.at("format").is_null()) {
        return;
    }
    const std::string   field  = prefix + ".format";
    const common_json & format = tool.at("format");
    if (!format.is_object()) {
        throw invalid_request_field(field, "invalid_type", "Invalid type for '" + field + "': expected an object.");
    }
    if (!format.contains("type")) {
        throw invalid_request_field(field + ".type", "missing_required_parameter",
                                    "Missing required parameter: '" + field + ".type'.");
    }
    if (!format.at("type").is_string()) {
        throw invalid_request_field(field + ".type", "invalid_type",
                                    "Invalid type for '" + field + ".type': expected a string.");
    }
    const std::string type = format.at("type").get<std::string>();
    if (type == "text") {
        return;
    }
    if (type != "grammar") {
        throw invalid_request_field(field + ".type", "invalid_value",
                                    "Invalid value for '" + field + ".type': expected 'text' or 'grammar'.");
    }
    if (!format.contains("syntax")) {
        throw invalid_request_field(field + ".syntax", "missing_required_parameter",
                                    "Missing required parameter: '" + field + ".syntax'.");
    }
    if (!format.at("syntax").is_string()) {
        throw invalid_request_field(field + ".syntax", "invalid_type",
                                    "Invalid type for '" + field + ".syntax': expected a string.");
    }
    const std::string syntax = format.at("syntax").get<std::string>();
    if (syntax != "lark" && syntax != "regex") {
        throw invalid_request_field(field + ".syntax", "invalid_value",
                                    "Invalid value for '" + field + ".syntax': expected 'lark' or 'regex'.");
    }
    if (!format.contains("definition")) {
        throw invalid_request_field(field + ".definition", "missing_required_parameter",
                                    "Missing required parameter: '" + field + ".definition'.");
    }
    if (!format.at("definition").is_string()) {
        throw invalid_request_field(field + ".definition", "invalid_type",
                                    "Invalid type for '" + field + ".definition': expected a string.");
    }
}

void validate_deferred_tool(const common_json & tool, const std::string & prefix) {
    validate_optional_tool_field(tool, "defer_loading", prefix, &common_json::is_boolean, "a boolean or null");
    if (tool.contains("defer_loading") && !tool.at("defer_loading").is_null() && tool.at("defer_loading").get<bool>()) {
        throw unsupported_request_field(prefix + ".defer_loading", "Deferred tool loading is not available");
    }
}

std::string validate_function_tool(const common_json & tool,
                                   const std::string & prefix,
                                   std::size_t         max_name_length = 64U) {
    const std::string name = require_tool_name(tool, prefix + ".name", max_name_length);
    validate_optional_tool_field(tool, "description", prefix, &common_json::is_string, "a string or null");
    validate_optional_tool_field(tool, "parameters", prefix, &common_json::is_object, "an object or null");
    validate_optional_tool_field(tool, "strict", prefix, &common_json::is_boolean, "a boolean or null");
    validate_deferred_tool(tool, prefix);
    // The real endpoint supplies defaults when `parameters` or `strict` is
    // omitted; the inference adapter follows that dated oracle.
    return name;
}

std::string validate_custom_tool(const common_json & tool,
                                 const std::string & prefix,
                                 std::size_t         max_name_length = 64U) {
    const std::string name = require_tool_name(tool, prefix + ".name", max_name_length);
    validate_optional_tool_field(tool, "description", prefix, &common_json::is_string, "a string or null");
    validate_custom_tool_format(tool, prefix);
    validate_deferred_tool(tool, prefix);
    return name;
}

struct declared_tools {
    std::set<std::string> functions;
    std::set<std::string> custom;
    std::set<std::string> namespaces;
    bool                  local_shell = false;

    bool empty() const { return functions.empty() && custom.empty() && namespaces.empty() && !local_shell; }
};

declared_tools validate_tools(common_json & request) {
    declared_tools declared;
    if (!request.contains("tools")) {
        return declared;
    }
    if (request.at("tools").is_null()) {
        request["tools"] = common_json::array();
        return declared;
    }
    if (!request.at("tools").is_array()) {
        throw invalid_request_field("tools", "invalid_type", "Invalid type for 'tools': expected an array or null.");
    }

    static const std::set<std::string> hosted_types = {
        "apply_patch",
        "code_interpreter",
        "computer",
        "computer_use_preview",
        "file_search",
        "image_generation",
        "mcp",
        "programmatic_tool_calling",
        "shell",
        "tool_search",
        "web_search",
        "web_search_preview",
        "web_search_preview_2025_03_11",
    };
    std::size_t index = 0;
    for (const common_json & tool : request.at("tools")) {
        const std::string prefix = "tools[" + std::to_string(index) + "]";
        if (!tool.is_object()) {
            throw invalid_request_field(prefix, "invalid_type",
                                        "Invalid type for '" + prefix + "': expected an object.");
        }
        if (!tool.contains("type")) {
            throw invalid_request_field(prefix + ".type", "missing_required_parameter",
                                        "Missing required parameter: '" + prefix + ".type'.");
        }
        if (!tool.at("type").is_string()) {
            throw invalid_request_field(prefix + ".type", "invalid_type",
                                        "Invalid type for '" + prefix + ".type': expected a string.");
        }
        const std::string type = tool.at("type").get<std::string>();
        if (type == "function") {
            const std::string name = validate_function_tool(tool, prefix);
            if (!declared.functions.insert(name).second || declared.custom.find(name) != declared.custom.end() ||
                declared.namespaces.find(name) != declared.namespaces.end() ||
                (declared.local_shell && name == "local_shell")) {
                throw invalid_request_field(prefix + ".name", "invalid_value", "Tool names must be unique.");
            }
        } else if (type == "custom") {
            const std::string name = validate_custom_tool(tool, prefix);
            if (!declared.custom.insert(name).second || declared.functions.find(name) != declared.functions.end() ||
                declared.namespaces.find(name) != declared.namespaces.end() ||
                (declared.local_shell && name == "local_shell")) {
                throw invalid_request_field(prefix + ".name", "invalid_value", "Tool names must be unique.");
            }
        } else if (type == "namespace") {
            const std::string name = require_tool_name(tool, prefix + ".name");
            validate_optional_tool_field(tool, "description", prefix, &common_json::is_string, "a string or null");
            if (!tool.contains("tools")) {
                throw invalid_request_field(prefix + ".tools", "missing_required_parameter",
                                            "Missing required parameter: '" + prefix + ".tools'.");
            }
            if (!tool.at("tools").is_array()) {
                throw invalid_request_field(prefix + ".tools", "invalid_type",
                                            "Invalid type for '" + prefix + ".tools': expected an array.");
            }
            if (!declared.namespaces.insert(name).second || declared.functions.find(name) != declared.functions.end() ||
                declared.custom.find(name) != declared.custom.end() ||
                (declared.local_shell && name == "local_shell")) {
                throw invalid_request_field(prefix + ".name", "invalid_value", "Tool names must be unique.");
            }

            std::set<std::string> nested_names;
            std::size_t           nested_index = 0;
            for (const common_json & nested : tool.at("tools")) {
                const std::string nested_prefix = prefix + ".tools[" + std::to_string(nested_index) + "]";
                if (!nested.is_object()) {
                    throw invalid_request_field(nested_prefix, "invalid_type",
                                                "Invalid type for '" + nested_prefix + "': expected an object.");
                }
                if (!nested.contains("type")) {
                    throw invalid_request_field(nested_prefix + ".type", "missing_required_parameter",
                                                "Missing required parameter: '" + nested_prefix + ".type'.");
                }
                if (!nested.at("type").is_string()) {
                    throw invalid_request_field(nested_prefix + ".type", "invalid_type",
                                                "Invalid type for '" + nested_prefix + ".type': expected a string.");
                }
                const std::string nested_type = nested.at("type").get<std::string>();
                std::string       nested_name;
                if (nested_type == "function") {
                    nested_name = validate_function_tool(nested, nested_prefix, 128U);
                } else if (nested_type == "custom") {
                    nested_name = validate_custom_tool(nested, nested_prefix, 128U);
                } else {
                    throw invalid_request_field(
                        nested_prefix + ".type", "invalid_value",
                        "Invalid value for '" + nested_prefix + ".type': expected 'function' or 'custom'.");
                }
                if (!nested_names.insert(nested_name).second) {
                    throw invalid_request_field(nested_prefix + ".name", "invalid_value",
                                                "Names within a tool namespace must be unique.");
                }
                ++nested_index;
            }
        } else if (type == "local_shell") {
            if (declared.local_shell || declared.functions.find("local_shell") != declared.functions.end() ||
                declared.custom.find("local_shell") != declared.custom.end() ||
                declared.namespaces.find("local_shell") != declared.namespaces.end()) {
                throw invalid_request_field(prefix + ".type", "invalid_value",
                                            "Only one local_shell tool may be declared.");
            }
            declared.local_shell = true;
        } else if (hosted_types.find(type) != hosted_types.end()) {
            throw unsupported_request_field(prefix + ".type",
                                            "The requested hosted tool '" + type + "' is not available");
        } else {
            throw invalid_request_field(prefix + ".type", "invalid_value",
                                        "Invalid value for '" + prefix + ".type': unknown tool type.");
        }
        ++index;
    }
    return declared;
}

void validate_tool_choice(common_json & request, const declared_tools & declared) {
    if (!request.contains("tool_choice") || request.at("tool_choice").is_null()) {
        request["tool_choice"] = "auto";
        return;
    }
    const common_json & choice = request.at("tool_choice");
    if (choice.is_string()) {
        const std::string value = choice.get<std::string>();
        if (value != "none" && value != "auto" && value != "required") {
            throw invalid_request_field("tool_choice", "invalid_value",
                                        "Invalid value for 'tool_choice': expected 'none', 'auto', or 'required'.");
        }
        if (value == "required" && declared.empty()) {
            throw invalid_request_field("tool_choice", "invalid_value",
                                        "Invalid value for 'tool_choice': no tools were declared.");
        }
        return;
    }
    if (!choice.is_object()) {
        throw invalid_request_field("tool_choice", "invalid_type",
                                    "Invalid type for 'tool_choice': expected a string, object, or null.");
    }
    if (!choice.contains("type")) {
        throw invalid_request_field("tool_choice.type", "missing_required_parameter",
                                    "Missing required parameter: 'tool_choice.type'.");
    }
    if (!choice.at("type").is_string()) {
        throw invalid_request_field("tool_choice.type", "invalid_type",
                                    "Invalid type for 'tool_choice.type': expected a string.");
    }
    const std::string type = choice.at("type").get<std::string>();
    if (type != "function" && type != "custom") {
        static const std::set<std::string> hosted_choices = {
            "allowed_tools",
            "apply_patch",
            "code_interpreter",
            "computer",
            "computer_use_preview",
            "file_search",
            "image_generation",
            "mcp",
            "namespace",
            "programmatic_tool_calling",
            "shell",
            "tool_search",
            "web_search",
            "web_search_preview",
            "web_search_preview_2025_03_11",
        };
        if (hosted_choices.find(type) != hosted_choices.end()) {
            throw unsupported_request_field("tool_choice.type",
                                            "The requested hosted tool choice '" + type + "' is not available");
        }
        throw invalid_request_field("tool_choice.type", "invalid_value",
                                    "Invalid value for 'tool_choice.type': unknown tool choice type.");
    }
    const std::string name  = require_tool_name(choice, "tool_choice.name");
    const bool        found = type == "function" ? declared.functions.find(name) != declared.functions.end() :
                                                   declared.custom.find(name) != declared.custom.end();
    if (!found) {
        throw invalid_request_field("tool_choice.name", "invalid_value",
                                    "Invalid value for 'tool_choice.name': no matching tool was declared.");
    }
}

void validate_include(const common_json & request) {
    if (!request.contains("include") || request.at("include").is_null()) {
        return;
    }
    const common_json & include = request.at("include");
    if (!include.is_array()) {
        throw invalid_request_field("include", "invalid_type",
                                    "Invalid type for 'include': expected an array of strings or null.");
    }
    for (const common_json & projection : include) {
        if (!projection.is_string()) {
            throw invalid_request_field("include", "invalid_type",
                                        "Invalid type for 'include': expected an array of strings or null.");
        }
        if (projection.get<std::string>() != "reasoning.encrypted_content") {
            throw unsupported_request_field("include", "The requested 'include' projection is not available");
        }
        // Codex requests this when it must replay opaque reasoning. Local
        // generation does not produce encrypted reasoning, so there is no
        // hidden payload to project; accepting the request is nevertheless
        // required for the ordinary Codex request path.
    }
}

void validate_stream_options(const common_json & request) {
    if (!request.contains("stream_options") || request.at("stream_options").is_null()) {
        return;
    }
    const common_json & options = request.at("stream_options");
    if (!options.is_object()) {
        throw invalid_request_field("stream_options", "invalid_type",
                                    "Invalid type for 'stream_options': expected an object or null.");
    }
    for (const auto & entry : options.items()) {
        if (entry.key() != "include_obfuscation") {
            const std::string field = "stream_options." + entry.key();
            throw invalid_request_field(field, "unknown_parameter", "Unknown parameter: '" + field + "'.");
        }
    }
    if (!request.value("stream", false)) {
        throw invalid_request_field("stream_options", "", "'stream_options' may only be used when 'stream' is true");
    }
    if (options.contains("include_obfuscation") && !options.at("include_obfuscation").is_boolean()) {
        throw invalid_request_field("stream_options.include_obfuscation", "invalid_type",
                                    "Invalid type for 'stream_options.include_obfuscation': expected a boolean.");
    }
    if (options.value("include_obfuscation", false)) {
        throw unsupported_request_field("stream_options.include_obfuscation", "Stream obfuscation is not available");
    }
}

void validate_reasoning(const common_json & request) {
    if (!request.contains("reasoning") || request.at("reasoning").is_null()) {
        return;
    }
    const common_json & reasoning = request.at("reasoning");
    if (!reasoning.is_object()) {
        throw invalid_request_field("reasoning", "invalid_type",
                                    "Invalid type for 'reasoning': expected an object or null.");
    }
    for (const auto & entry : reasoning.items()) {
        const std::string field = "reasoning." + entry.key();
        if (entry.key() != "effort" && entry.key() != "summary" && entry.key() != "generate_summary" &&
            entry.key() != "context" && entry.key() != "mode") {
            throw invalid_request_field(field, "unknown_parameter", "Unknown parameter: '" + field + "'.");
        }
    }
    if (reasoning.contains("effort") && !reasoning.at("effort").is_null() && !reasoning.at("effort").is_string()) {
        throw invalid_request_field(
            "reasoning.effort", "invalid_type",
            "Invalid type for 'reasoning.effort': expected a model-supported effort string or null.");
    }
    if (reasoning.contains("effort") && reasoning.at("effort").is_string()) {
        const std::string effort = reasoning.at("effort").get<std::string>();
        if (effort != "none" && effort != "minimal" && effort != "low" && effort != "medium" && effort != "high" &&
            effort != "xhigh" && effort != "max") {
            throw invalid_request_field(
                "reasoning.effort", "invalid_value",
                "Invalid value for 'reasoning.effort': expected a globally recognized reasoning effort.");
        }
    }
    if (reasoning.contains("summary") && !reasoning.at("summary").is_null()) {
        throw unsupported_request_field("reasoning.summary", "Reasoning summaries are not available");
    }
    if (reasoning.contains("generate_summary") && !reasoning.at("generate_summary").is_null()) {
        throw unsupported_request_field("reasoning.generate_summary", "Reasoning summaries are not available");
    }
    if (reasoning.contains("context") && !reasoning.at("context").is_null()) {
        throw unsupported_request_field("reasoning.context", "Opaque reasoning context is not available");
    }
    if (reasoning.contains("mode") && !reasoning.at("mode").is_null()) {
        throw unsupported_request_field("reasoning.mode", "Reasoning mode selection is not available");
    }
}

void validate_text(common_json & request) {
    if (!request.contains("text") || request.at("text").is_null()) {
        request["text"] = common_json::object();
    }
    common_json & text = request["text"];
    if (!text.is_object()) {
        throw invalid_request_field("text", "invalid_type", "Invalid type for 'text': expected an object.");
    }
    for (const auto & entry : text.items()) {
        if (entry.key() != "format" && entry.key() != "verbosity") {
            const std::string field = "text." + entry.key();
            throw invalid_request_field(field, "unknown_parameter", "Unknown parameter: '" + field + "'.");
        }
    }

    if (!text.contains("format") || text.at("format").is_null()) {
        text["format"] = {
            { "type", "text" },
        };
    } else {
        common_json & format = text["format"];
        if (!format.is_object()) {
            throw invalid_request_field("text.format", "invalid_type",
                                        "Invalid type for 'text.format': expected an object or null.");
        }
        if (!format.contains("type")) {
            throw invalid_request_field("text.format.type", "missing_required_parameter",
                                        "Missing required parameter: 'text.format.type'.");
        }
        if (!format.at("type").is_string()) {
            throw invalid_request_field(
                "text.format.type", "invalid_value",
                "Invalid value for 'text.format.type': expected 'text', 'json_object', or 'json_schema'.");
        }
        const std::string type = format.at("type").get<std::string>();
        if (type != "text" && type != "json_object" && type != "json_schema") {
            throw invalid_request_field(
                "text.format.type", "invalid_value",
                "Invalid value for 'text.format.type': expected 'text', 'json_object', or 'json_schema'.");
        }

        for (const auto & entry : format.items()) {
            const bool json_schema_field =
                type == "json_schema" && (entry.key() == "name" || entry.key() == "description" ||
                                          entry.key() == "schema" || entry.key() == "strict");
            if (entry.key() != "type" && !json_schema_field) {
                const std::string field = "text.format." + entry.key();
                throw invalid_request_field(field, "unknown_parameter", "Unknown parameter: '" + field + "'.");
            }
        }
        if (type == "json_schema") {
            if (!format.contains("name")) {
                throw invalid_request_field("text.format.name", "missing_required_parameter",
                                            "Missing required parameter: 'text.format.name'.");
            }
            if (!format.at("name").is_string()) {
                throw invalid_request_field("text.format.name", "invalid_type",
                                            "Invalid type for 'text.format.name': expected a string.");
            }
            if (!format.contains("schema")) {
                throw invalid_request_field("text.format.schema", "missing_required_parameter",
                                            "Missing required parameter: 'text.format.schema'.");
            }
            if (!format.at("schema").is_object()) {
                throw invalid_request_field("text.format.schema", "invalid_type",
                                            "Invalid type for 'text.format.schema': expected an object.");
            }
            if (format.contains("description") && !format.at("description").is_null() &&
                !format.at("description").is_string()) {
                throw invalid_request_field("text.format.description", "invalid_type",
                                            "Invalid type for 'text.format.description': expected a string or null.");
            }
            if (format.contains("strict") && !format.at("strict").is_null() && !format.at("strict").is_boolean()) {
                throw invalid_request_field("text.format.strict", "invalid_type",
                                            "Invalid type for 'text.format.strict': expected a boolean or null.");
            }
        }
    }

    if (!text.contains("verbosity") || text.at("verbosity").is_null()) {
        text["verbosity"] = "medium";
    } else {
        throw unsupported_request_field("text.verbosity", "Text verbosity control is not available");
    }
}

void reject_non_null_field(const common_json & request, const char * key, const char * message) {
    if (request.contains(key) && !request.at(key).is_null()) {
        throw unsupported_request_field(key, message);
    }
}

void validate_create_policy(common_json & request) {
    validate_top_level_fields(request);
    normalize_nullable_defaults(request);
    if (request.contains("stream") && !request.at("stream").is_boolean()) {
        throw invalid_request_field("stream", "invalid_type", "Invalid type for 'stream': expected a boolean.");
    }
    validate_metadata(request);
    validate_client_metadata(request);
    const declared_tools tools = validate_tools(request);
    validate_tool_choice(request, tools);
    validate_include(request);
    validate_stream_options(request);
    validate_reasoning(request);
    validate_text(request);
    validate_optional_identifier(request, "user", 64);
    validate_optional_identifier(request, "safety_identifier", 64);
    validate_optional_identifier(request, "prompt_cache_key", 64);

    if (request.contains("max_output_tokens") && !request.at("max_output_tokens").is_null()) {
        if (!request.at("max_output_tokens").is_number_integer()) {
            throw invalid_request_field("max_output_tokens", "invalid_type",
                                        "Invalid 'max_output_tokens': expected an integer.");
        }
        bool below_minimum = false;
        if (json_integer_is_negative(request.at("max_output_tokens"))) {
            below_minimum = request.at("max_output_tokens").get<std::int64_t>() < 16;
        } else {
            below_minimum = request.at("max_output_tokens").get<std::uint64_t>() < 16U;
        }
        if (below_minimum) {
            throw invalid_request_field(
                "max_output_tokens", "integer_below_min_value",
                "Invalid 'max_output_tokens': integer below minimum value. Expected a value >= 16.");
        }
    }

    if (request.contains("truncation") && !request.at("truncation").is_null()) {
        if (!request.at("truncation").is_string()) {
            throw invalid_request_field("truncation", "invalid_type",
                                        "Invalid type for 'truncation': expected a string or null.");
        }
        const std::string truncation = request.at("truncation").get<std::string>();
        if (truncation == "auto") {
            throw unsupported_request_field("truncation", "Automatic input truncation is not available");
        }
        if (truncation != "disabled") {
            throw invalid_request_field("truncation", "invalid_value",
                                        "Invalid value for 'truncation': expected 'disabled' or 'auto'.");
        }
    }

    if (request.contains("service_tier") && !request.at("service_tier").is_null()) {
        if (!request.at("service_tier").is_string()) {
            throw invalid_request_field("service_tier", "invalid_type",
                                        "Invalid type for 'service_tier': expected a string or null.");
        }
        const std::string tier = request.at("service_tier").get<std::string>();
        if (tier != "auto" && tier != "default") {
            throw unsupported_request_field("service_tier", "The requested service tier is not available");
        }
        // This local deployment has one actual tier. OpenAI responses report
        // the tier used, which need not equal the requested `auto` policy.
        request["service_tier"] = "default";
    }

    if (request.contains("top_logprobs") && !request.at("top_logprobs").is_null()) {
        if (!request.at("top_logprobs").is_number_integer()) {
            throw invalid_request_field("top_logprobs", "invalid_type",
                                        "Invalid type for 'top_logprobs': expected an integer or null.");
        }
        std::uint64_t top_logprobs = 0;
        if (json_integer_is_negative(request.at("top_logprobs"))) {
            const std::int64_t signed_value = request.at("top_logprobs").get<std::int64_t>();
            if (signed_value < 0) {
                throw invalid_request_field(
                    "top_logprobs", "integer_below_min_value",
                    "Invalid 'top_logprobs': integer below minimum value. Expected a value >= 0.");
            }
            top_logprobs = static_cast<std::uint64_t>(signed_value);
        } else {
            top_logprobs = request.at("top_logprobs").get<std::uint64_t>();
        }
        if (top_logprobs > 20U) {
            throw invalid_request_field("top_logprobs", "integer_above_max_value",
                                        "Invalid 'top_logprobs': integer above maximum value. Expected a value <= 20.");
        }
        if (top_logprobs > 0) {
            throw unsupported_request_field("top_logprobs", "Responses logprobs are not available");
        }
    }

    if (request.contains("context_management") && !request.at("context_management").is_null()) {
        if (!request.at("context_management").is_array()) {
            throw invalid_request_field("context_management", "invalid_type",
                                        "Invalid type for 'context_management': expected an array of objects.");
        }
        if (request.at("context_management").empty()) {
            throw invalid_request_field(
                "context_management", "empty_array",
                "Invalid 'context_management': empty array. Expected an array with minimum length 1.");
        }
        throw unsupported_request_field("context_management", "Automatic context management is not available");
    }
    reject_non_null_field(request, "prompt", "Reusable prompt resources are not available");
    reject_non_null_field(request, "moderation", "Response moderation is not available");
    reject_non_null_field(request, "max_tool_calls", "Server-hosted tool call limits are not available");
    reject_non_null_field(request, "prompt_cache_options", "Explicit prompt-cache options are not available");
    reject_non_null_field(request, "prompt_cache_retention", "Prompt-cache retention policy is not available");
    reject_non_null_field(request, "personality", "Model personality selection is not available");
    reject_non_null_field(request, "web_search", "Server-hosted web search is not available");
}

void validate_unique_input_item_ids(const common_json & items) {
    std::set<std::string> ids;
    for (const common_json & item : items) {
        const std::string id = item.is_object() ? item.value("id", std::string()) : std::string();
        if (id.empty() || !ids.insert(id).second) {
            throw invalid_request_field(
                "input", "invalid_value",
                "Responses input item ids must be non-empty and unique after continuation expansion.");
        }
    }
}

class responses_routes_impl final : public std::enable_shared_from_this<responses_routes_impl> {
  public:
    explicit responses_routes_impl(server_responses_routes legacy) :
        legacy(std::move(legacy)),
        store(std::make_unique<sqlite_response_store>(default_sqlite_response_store_path())),
        resources(*store) {}

    static server_responses_routes routes(const std::shared_ptr<responses_routes_impl> & self) {
        server_responses_routes result;
        result.owner    = self;
        result.generate = self->legacy.generate;
        result.create   = [self](const server_http_req & request) {
            return self->create(request);
        };
        result.input_tokens = [self](const server_http_req & request) {
            return self->input_tokens(request);
        };
        result.retrieve = [self](const server_http_req & request) {
            return self->retrieve(request);
        };
        result.delete_response = [self](const server_http_req & request) {
            return self->erase(request);
        };
        result.cancel = [](const server_http_req & request) {
            return responses_routes_impl::cancel(request);
        };
        result.compact = [](const server_http_req & request) {
            return responses_routes_impl::compact(request);
        };
        result.input_items = [self](const server_http_req & request) {
            return self->input_items(request);
        };
        return result;
    }

  private:
    struct prepared_request {
        std::shared_ptr<server_http_req> request;
        common_json                      original                 = common_json::object();
        common_json                      continuation_input_items = common_json::array();
        common_json                      materialized_input_items = common_json::array();
    };

    server_responses_routes         legacy;
    std::unique_ptr<response_store> store;
    response_resource_service       resources;
    // The registry establishes the native C++ strategy boundary now. Its
    // default providers are deliberately unavailable until Phase 4 adapters
    // are configured.
    hosted_tool_registry            hosted_tools;

    static item_id make_input_id(std::size_t /*index*/, const common_json & item) {
        std::string prefix = "item_";
        if (item.is_object() && item.value("type", std::string()) == "message") {
            prefix = "msg_";
        }
        return item_id(prefix + random_id_suffix());
    }

    void resolve_item_references(common_json & input) const {
        if (input.is_object()) {
            if (input.value("type", std::string()) != "item_reference") {
                return;
            }
            const std::string id = input.value("id", std::string());
            if (id.empty()) {
                throw std::invalid_argument("item_reference_not_found: id=<unset>");
            }
            const auto resolved = store->find_item(item_id(id));
            if (!resolved) {
                throw std::invalid_argument("item_reference_not_found: id=" + id);
            }
            input = render_output_item(resolved->item);
            return;
        }
        if (!input.is_array()) {
            return;
        }
        for (common_json & item : input) {
            resolve_item_references(item);
        }
    }

    void append_response_context(const response_id & previous, common_json & expanded) const {
        // Every stored response owns a fully materialized input snapshot. One
        // read is therefore sufficient and remains valid if an ancestor (or
        // this parent) is deleted before the new terminal response is stored.
        const auto state = store->find(previous);
        if (!state) {
            throw std::out_of_range(previous.str());
        }
        for (const common_json & item : state->input_items) {
            expanded.push_back(item);
        }
        for (const response_output_item & item : state->output) {
            expanded.push_back(render_output_item(item));
        }
    }

    prepared_request prepare(const server_http_req & request) {
        common_json original = parse_json_body(request);
        validate_create_policy(original);
        if (original.contains("store") && !original.at("store").is_null() && !original.at("store").is_boolean()) {
            throw invalid_request_field("store", "invalid_type",
                                        "Invalid type for 'store': expected a boolean or null.");
        }
        if (original.contains("background") && !original.at("background").is_boolean()) {
            throw invalid_request_field("background", "invalid_type",
                                        "Invalid type for 'background': expected a boolean.");
        }
        if (original.value("background", false)) {
            throw unsupported_request_field("background",
                                            "Background responses are not available in the foreground server profile");
        }
        if (original.contains("conversation") && !original.at("conversation").is_null()) {
            throw unsupported_request_field(
                "conversation", "Conversation resources are not available in the persistent foreground profile");
        }
        if (original.contains("instructions") && !original.at("instructions").is_null() &&
            !original.at("instructions").is_string() && !original.at("instructions").is_array()) {
            throw invalid_request_field(
                "instructions", "invalid_type",
                "Invalid type for 'instructions': expected a string, an array of items, or null.");
        }
        std::string previous;
        if (original.contains("previous_response_id") && !original.at("previous_response_id").is_null()) {
            if (!original.at("previous_response_id").is_string()) {
                throw std::invalid_argument("'previous_response_id' must be a string or null");
            }
            previous = original.at("previous_response_id").get<std::string>();
        }
        if (!original.contains("input") && previous.empty()) {
            throw std::invalid_argument("'input' is required");
        }
        if (original.contains("input") && original.at("input").is_null()) {
            throw std::invalid_argument("'input' must be a string, object, or array of items");
        }

        common_json normalized = original;
        if (normalized.contains("input")) {
            resolve_item_references(normalized["input"]);
        }
        if (normalized.contains("instructions") && normalized.at("instructions").is_array()) {
            resolve_item_references(normalized["instructions"]);
        }
        common_json current_items = capture_input_items(
            normalized, [](std::size_t index, const common_json & item) { return make_input_id(index, item); });
        validate_unique_input_item_ids(current_items);
        common_json expanded = common_json::array();

        if (!previous.empty()) {
            append_response_context(response_id(previous), expanded);
        }
        for (const auto & item : current_items) {
            expanded.push_back(item);
        }
        validate_unique_input_item_ids(expanded);

        common_json forwarded = original;
        forwarded["input"]    = expanded;
        // `top_logprobs: 0` is a valid Responses default, but the legacy
        // Chat-shaped inference adapter interprets any present value as a
        // request for Chat logprobs. The public value remains in `original`.
        forwarded.erase("top_logprobs");
        forwarded.erase("stream_options");
        forwarded.erase("client_metadata");
        if (forwarded.contains("tools") && forwarded.at("tools").is_array()) {
            for (common_json & tool : forwarded["tools"]) {
                if (tool.is_object()) {
                    // Codex telemetry/tool-loading policy belongs to the
                    // Responses boundary, not the Chat-shaped inference DTO.
                    tool.erase("defer_loading");
                }
            }
        }
        if (forwarded.contains("max_output_tokens") && forwarded.at("max_output_tokens").is_number_integer() &&
            !json_integer_is_negative(forwarded.at("max_output_tokens")) &&
            forwarded.at("max_output_tokens").get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            // The shared inference runtime uses an int token budget. Values
            // above that are observationally unbounded for a local context;
            // retain the caller's 64-bit value in `original` for the envelope.
            forwarded["max_output_tokens"] = std::numeric_limits<int>::max();
        }
        if (normalized.contains("instructions")) {
            forwarded["instructions"] = normalized.at("instructions");
        }
        // The legacy generation adapter lowers the expanded items to the model,
        // while this hidden field keeps the public response envelope tied to the
        // caller's unexpanded request.
        forwarded["__llama_responses_request"] = original;

        auto forwarded_request  = std::make_shared<server_http_req>(request);
        forwarded_request->body = forwarded.dump();
        return {
            std::move(forwarded_request),
            std::move(original),
            std::move(current_items),
            std::move(expanded),
        };
    }

    bool capture_response(const common_json & wire_response,
                          const common_json & original,
                          const common_json & continuation_input_items,
                          const common_json & materialized_input_items) {
        if (!request_store_enabled(original)) {
            return true;
        }
        try {
            response_state state            = capture_response_state(wire_response, original, materialized_input_items);
            state.continuation_input_items  = continuation_input_items;
            const store_write_result result = store->create(std::move(state));
            if (result != store_write_result::stored) {
                LOG_WRN("llama-responses: response storage failed: %s\n", store_write_result_name(result));
                return false;
            }
        } catch (const std::exception & error) {
            LOG_WRN("llama-responses: response storage failed: %s\n", error.what());
            return false;
        }
        return true;
    }

    std::optional<std::uint64_t> capture_sse(const std::string & payload,
                                             const common_json & original,
                                             const common_json & continuation_input_items,
                                             const common_json & materialized_input_items) {
        std::size_t start = 0;
        while (start < payload.size()) {
            const std::size_t end  = payload.find('\n', start);
            std::string       line = payload.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.rfind("data:", 0) == 0) {
                const std::string data = strip_ascii_space(line.substr(5));
                if (!data.empty() && data != "[DONE]") {
                    try {
                        const common_json event = common_json::parse(data);
                        const std::string type  = event.value("type", std::string());
                        if ((type == "response.completed" || type == "response.incomplete" ||
                             type == "response.failed" || type == "response.cancelled") &&
                            event.contains("response")) {
                            if (!capture_response(event.at("response"), original, continuation_input_items,
                                                  materialized_input_items)) {
                                return event.value("sequence_number", std::uint64_t{ 0 });
                            }
                        }
                    } catch (const std::exception & error) {
                        // A stored stream cannot acknowledge a terminal frame
                        // whose resource snapshot was not decoded and committed.
                        LOG_WRN("llama-responses: malformed SSE data prevented response storage: %s\n", error.what());
                        return std::uint64_t{ 0 };
                    }
                }
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return std::nullopt;
    }

    static std::string storage_error_sse(std::uint64_t sequence_number) {
        const common_json event = {
            { "type",            "error"                                         },
            { "sequence_number", sequence_number                                 },
            { "code",            "response_store_error"                          },
            { "message",         "The generated response could not be persisted" },
            { "param",           nullptr                                         },
        };
        return "event: error\ndata: " + event.dump() + "\n\n";
    }

    void filter_sse_chunk(std::string &       pending,
                          std::string &       output,
                          bool &              storage_failed,
                          const common_json & original,
                          const common_json & continuation_input_items,
                          const common_json & materialized_input_items) {
        if (storage_failed) {
            output.clear();
            return;
        }

        pending += output;
        output.clear();
        while (true) {
            const std::size_t lf_boundary   = pending.find("\n\n");
            const std::size_t crlf_boundary = pending.find("\r\n\r\n");
            std::size_t       boundary      = std::string::npos;
            std::size_t       delimiter     = 0;
            if (lf_boundary != std::string::npos &&
                (crlf_boundary == std::string::npos || lf_boundary < crlf_boundary)) {
                boundary  = lf_boundary;
                delimiter = 2;
            } else if (crlf_boundary != std::string::npos) {
                boundary  = crlf_boundary;
                delimiter = 4;
            }
            if (boundary == std::string::npos) {
                return;
            }

            const std::size_t frame_size = boundary + delimiter;
            const std::string frame      = pending.substr(0, frame_size);
            pending.erase(0, frame_size);
            const auto failed_sequence =
                capture_sse(frame, original, continuation_input_items, materialized_input_items);
            if (failed_sequence) {
                output += storage_error_sse(*failed_sequence);
                pending.clear();
                storage_failed = true;
                return;
            }
            output += frame;
        }
    }

    server_http_res_ptr create(const server_http_req & request) {
        prepared_request prepared;
        try {
            prepared = prepare(request);
        } catch (const invalid_request_field & error) {
            return api_error(400, error.what(), error.code, error.field);
        } catch (const unsupported_request_field & error) {
            return api_error(400, error.what(), "unsupported_parameter", error.field);
        } catch (const std::out_of_range & error) {
            return json_response(render_response_not_found(response_id(error.what())), 404);
        } catch (const std::exception & error) {
            return api_error(400, error.what(), "invalid_request");
        }

        if (legacy.generate && native_generation_supported(prepared.original)) {
            return create_native(prepared);
        }
        if (!legacy.create) {
            return api_error(501, "Responses create is not available", "not_supported");
        }
        return create_legacy(std::move(prepared));
    }

    static generation_response_context native_context(const prepared_request & prepared) {
        generation_response_context context;
        context.model                    = prepared.original.value("model", std::string());
        context.request                  = prepared.original;
        context.input_items              = prepared.materialized_input_items;
        context.continuation_input_items = prepared.continuation_input_items;
        context.created_at               = static_cast<std::uint64_t>(std::time(nullptr));
        if (prepared.original.contains("previous_response_id") &&
            prepared.original.at("previous_response_id").is_string()) {
            const std::string previous = prepared.original.at("previous_response_id").get<std::string>();
            if (!previous.empty()) {
                context.previous_response = response_id(previous);
            }
        }
        return context;
    }

    server_http_res_ptr create_native(const prepared_request & prepared) {
        const bool       stream  = prepared.original.value("stream", false);
        response_store * storage = request_store_enabled(prepared.original) ? store.get() : nullptr;
        auto sink = std::make_shared<native_server_generation_sink>(native_context(prepared), random_id_suffix(),
                                                                    stream, storage);

        server_http_res_ptr response;
        try {
            response = legacy.generate(*prepared.request, sink);
        } catch (const std::invalid_argument & error) {
            sink->discard_persisted_state();
            return api_error(400, error.what(), "invalid_request");
        } catch (const std::exception & error) {
            sink->discard_persisted_state();
            return api_error(500, error.what(), "server_error");
        }
        if (!response) {
            sink->discard_persisted_state();
            return api_error(500, "Responses generation returned no response", "server_error");
        }
        response->lifetime_owner = prepared.request;
        if (sink->storage_failed() && !response->is_stream()) {
            return api_error(500, sink->storage_error(), "response_store_error");
        }
        if (response->status < 200 || response->status >= 300) {
            sink->discard_persisted_state();
            return response;
        }
        return response;
    }

    server_http_res_ptr create_legacy(prepared_request prepared) {
        server_http_res_ptr response;
        try {
            response = legacy.create(*prepared.request);
        } catch (const std::invalid_argument & error) {
            return api_error(400, error.what(), "invalid_request");
        } catch (const std::exception & error) {
            return api_error(500, error.what(), "server_error");
        }
        if (!response) {
            return api_error(500, "Responses generation returned no response", "server_error");
        }
        response->lifetime_owner = prepared.request;
        if (!response->is_stream()) {
            if (request_store_enabled(prepared.original) && response->status >= 200 && response->status < 300 &&
                !response->data.empty()) {
                try {
                    if (!capture_response(common_json::parse(response->data), prepared.original,
                                          prepared.continuation_input_items, prepared.materialized_input_items)) {
                        return api_error(500, "The generated response could not be persisted", "response_store_error");
                    }
                } catch (const std::exception & error) {
                    LOG_WRN("llama-responses: malformed generated response could not be stored: %s\n", error.what());
                    return api_error(500, "The generated response could not be persisted", "response_store_error");
                }
            }
            return response;
        }

        if (request_store_enabled(prepared.original)) {
            response->chunk_filter = [self = shared_from_this(), original = std::move(prepared.original),
                                      continuation_input_items = std::move(prepared.continuation_input_items),
                                      materialized_input_items = std::move(prepared.materialized_input_items),
                                      pending = std::string(), storage_failed = false](std::string & output) mutable {
                self->filter_sse_chunk(pending, output, storage_failed, original, continuation_input_items,
                                       materialized_input_items);
            };
        }
        return response;
    }

    server_http_res_ptr input_tokens(const server_http_req & request) {
        if (!legacy.input_tokens) {
            return api_error(501, "Responses input_tokens is not available", "not_supported");
        }
        try {
            prepared_request prepared = prepare(request);
            return legacy.input_tokens(*prepared.request);
        } catch (const invalid_request_field & error) {
            return api_error(400, error.what(), error.code, error.field);
        } catch (const unsupported_request_field & error) {
            return api_error(400, error.what(), "unsupported_parameter", error.field);
        } catch (const std::out_of_range & error) {
            return json_response(render_response_not_found(response_id(error.what())), 404);
        } catch (const std::exception & error) {
            return api_error(400, error.what(), "invalid_request");
        }
    }

    server_http_res_ptr retrieve(const server_http_req & request) {
        const std::string stream = request.get_param("stream");
        if (!stream.empty() && stream != "false" && stream != "true") {
            return api_error(400, "Invalid retrieve stream parameter", "invalid_parameter", std::string("stream"));
        }
        if (stream == "true" || !request.get_param("starting_after").empty() || !request.get_param("include").empty() ||
            request.get_param("include_obfuscation") == "true") {
            return api_error(
                501, "Streaming or projected response retrieval is not available in the persistent foreground profile",
                "not_supported");
        }
        const resource_result result = resources.retrieve(response_id(response_id_param(request)));
        return json_response(result.body, status_for(result.kind));
    }

    server_http_res_ptr erase(const server_http_req & request) {
        const resource_result result = resources.erase(response_id(response_id_param(request)));
        return json_response(result.body, status_for(result.kind));
    }

    static server_http_res_ptr cancel(const server_http_req & /*request*/) {
        return api_error(501, "Response cancellation is not available in the foreground-only server profile",
                         "not_supported");
    }

    static server_http_res_ptr compact(const server_http_req & /*request*/) {
        return api_error(501, "Response compaction is not available in the persistent foreground profile",
                         "not_supported");
    }

    server_http_res_ptr input_items(const server_http_req & request) {
        if (!request.get_param("include").empty()) {
            return api_error(501, "Projected input-item retrieval is not available", "not_supported");
        }
        input_item_page_options options;
        const std::string       limit = request.get_param("limit");
        if (!limit.empty()) {
            try {
                std::size_t parsed = 0;
                options.limit      = static_cast<std::size_t>(std::stoull(limit, &parsed));
                if (parsed != limit.size()) {
                    throw std::invalid_argument("invalid limit");
                }
            } catch (const std::exception & error) {
                LOG_DBG("llama-responses: invalid input-items limit: %s\n", error.what());
                return api_error(400, "Invalid input-items pagination limit", "invalid_parameter",
                                 std::string("limit"));
            }
        }
        const std::string order = request.get_param("order", "desc");
        if (order != "asc" && order != "desc") {
            return api_error(400, "Invalid input-items pagination order", "invalid_parameter", std::string("order"));
        }
        options.order           = order == "asc" ? input_item_order::ascending : input_item_order::descending;
        const std::string after = request.get_param("after");
        if (!after.empty()) {
            options.after = item_id(after);
        }
        const resource_result result = resources.list_input_items(response_id(response_id_param(request)), options);
        return json_response(result.body, status_for(result.kind));
    }
};

}  // namespace

server_responses_routes_factory make_server_responses_routes_factory() {
    return [](server_responses_routes legacy) {
        auto implementation = std::make_shared<responses_routes_impl>(std::move(legacy));
        return responses_routes_impl::routes(implementation);
    };
}

server_route_extensions make_server_route_extensions() {
    server_route_extensions extensions;
    extensions.responses = make_server_responses_routes_factory();
    extensions.v1_models = make_codex_models_route_decorator();
    return extensions;
}

}  // namespace llama_responses
