#include "server-integration.h"

#include "codex-models.h"
#include "generation.h"
#include "hosted-tools.h"
#include "input-lowering.h"
#include "json.h"
#include "log.h"
#include "protocol-codec.h"
#include "response-service.h"
#include "response-store.h"
#include "response-types.h"
#include "server-generation-adapter.h"
#include "server-generation.h"
#include "server-http.h"
#include "server-responses.h"
#include "server-route-extensions.h"
#include "sqlite-response-store.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

std::string response_id_param(const server_http_req & request) {
    return request.get_param("response_id");
}

int query_hex_digit(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::string decode_query_component(const std::string & encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (encoded[index] == '%' && index + 2U < encoded.size()) {
            const int high = query_hex_digit(encoded[index + 1U]);
            const int low  = query_hex_digit(encoded[index + 2U]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2U;
                continue;
            }
        }
        decoded.push_back(encoded[index]);
    }
    return decoded;
}

std::vector<std::string> query_array_values(const server_http_req & request, const std::string & name) {
    std::vector<std::string> values;
    std::size_t              offset = 0;
    while (!request.query_string.empty() && offset <= request.query_string.size()) {
        const std::size_t separator = request.query_string.find('&', offset);
        const std::string field     = request.query_string.substr(
            offset, separator == std::string::npos ? std::string::npos : separator - offset);
        const std::size_t equals = field.find('=');
        const std::string key    = decode_query_component(field.substr(0, equals));
        if (key == name || key == name + "[]") {
            values.push_back(equals == std::string::npos ? std::string() :
                                                           decode_query_component(field.substr(equals + 1U)));
        }
        if (separator == std::string::npos) {
            break;
        }
        offset = separator + 1U;
    }
    if (values.empty()) {
        const std::string flattened = request.get_param(name);
        if (!flattened.empty()) {
            values.push_back(flattened);
        }
    }
    return values;
}

common_json parse_json_body(const server_http_req & request) {
    common_json body = common_json::parse(request.body);
    if (!body.is_object()) {
        throw std::invalid_argument("Responses request body must be a JSON object");
    }
    return body;
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

common_json canonical_json_value(const common_json & value) {
    if (value.is_array()) {
        common_json result = common_json::array();
        for (const common_json & element : value) {
            result.push_back(canonical_json_value(element));
        }
        return result;
    }
    if (!value.is_object()) {
        return value;
    }

    std::map<std::string, common_json> sorted;
    for (const auto & entry : value.items()) {
        sorted.emplace(entry.key(), canonical_json_value(entry.value()));
    }
    common_json result = common_json::object();
    for (auto & [key, element] : sorted) {
        result[key] = std::move(element);
    }
    return result;
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
    set_default("parallel_tool_calls", true);
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

std::string optional_string_field(const common_json & value, const char * name) {
    return value.is_object() && value.contains(name) && value.at(name).is_string() ? value.at(name).get<std::string>() :
                                                                                     std::string();
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
    bool                  tool_search = false;

    bool has_required_choice_target() const {
        return !functions.empty() || !custom.empty() || !namespaces.empty() || local_shell;
    }
};

void validate_client_tool_search(const common_json & tool, const std::string & prefix) {
    validate_optional_tool_field(tool, "description", prefix, &common_json::is_string, "a string or null");
    validate_optional_tool_field(tool, "parameters", prefix, &common_json::is_object, "an object or null");
    if (!tool.contains("execution") || tool.at("execution").is_null()) {
        throw unsupported_request_field(prefix + ".execution",
                                        "Hosted tool search is not available; use execution 'client'");
    }
    if (!tool.at("execution").is_string()) {
        throw invalid_request_field(prefix + ".execution", "invalid_type",
                                    "Invalid type for '" + prefix + ".execution': expected a string.");
    }
    const std::string execution = tool.at("execution").get<std::string>();
    if (execution == "server") {
        throw unsupported_request_field(prefix + ".execution", "Hosted tool search is not available");
    }
    if (execution != "client") {
        throw invalid_request_field(prefix + ".execution", "invalid_value",
                                    "Invalid value for '" + prefix + ".execution': expected 'client' or 'server'.");
    }
}

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
                (declared.local_shell && name == "local_shell") || (declared.tool_search && name == "tool_search")) {
                throw invalid_request_field(prefix + ".name", "invalid_value", "Tool names must be unique.");
            }
        } else if (type == "custom") {
            const std::string name = validate_custom_tool(tool, prefix);
            if (!declared.custom.insert(name).second || declared.functions.find(name) != declared.functions.end() ||
                declared.namespaces.find(name) != declared.namespaces.end() ||
                (declared.local_shell && name == "local_shell") || (declared.tool_search && name == "tool_search")) {
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
                (declared.local_shell && name == "local_shell") || (declared.tool_search && name == "tool_search")) {
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
        } else if (type == "tool_search") {
            validate_client_tool_search(tool, prefix);
            if (declared.tool_search || declared.functions.find("tool_search") != declared.functions.end() ||
                declared.custom.find("tool_search") != declared.custom.end() ||
                declared.namespaces.find("tool_search") != declared.namespaces.end()) {
                throw invalid_request_field(prefix + ".type", "invalid_value",
                                            "Only one client tool_search tool may be declared.");
            }
            declared.tool_search = true;
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

std::string tool_declaration_identity(const common_json & tool) {
    std::string type = optional_string_field(tool, "type");
    if (type == "local_shell" || type == "tool_search") {
        return type;
    }
    const std::string name           = optional_string_field(tool, "name");
    const std::string namespace_name = optional_string_field(tool, "namespace");
    return namespace_name.empty() ? name : namespace_name + '\n' + name;
}

struct effective_tool_definition {
    common_json comparable;
    std::size_t index = 0;
};

common_json comparable_namespace_shell(common_json tool) {
    tool.erase("tools");
    return canonical_json_value(tool);
}

void merge_namespace_declaration(common_json & target, const common_json & incoming, const std::string & prefix) {
    if (comparable_namespace_shell(target) != comparable_namespace_shell(incoming)) {
        throw invalid_request_field(
            prefix, "invalid_value",
            "Conflicting namespace declarations were supplied for '" + optional_string_field(incoming, "name") + "'.");
    }

    std::map<std::string, common_json> members;
    for (const common_json & member : target.at("tools")) {
        members.emplace(optional_string_field(member, "name"), canonical_json_value(member));
    }
    std::size_t member_index = 0;
    for (const common_json & member : incoming.at("tools")) {
        const std::string name       = optional_string_field(member, "name");
        const common_json comparable = canonical_json_value(member);
        const auto        existing   = members.find(name);
        if (existing == members.end()) {
            members.emplace(name, comparable);
            target["tools"].push_back(member);
        } else if (existing->second != comparable) {
            throw invalid_request_field(prefix + ".tools[" + std::to_string(member_index) + "]", "invalid_value",
                                        "Conflicting namespace member declarations were supplied for '" + name + "'.");
        }
        ++member_index;
    }
}

void append_effective_tool(common_json &                                      effective,
                           std::map<std::string, effective_tool_definition> & definitions,
                           const common_json &                                tool,
                           const std::string &                                prefix) {
    const std::string identity = tool_declaration_identity(tool);
    if (identity.empty()) {
        // The declaration validator will produce the precise missing/type
        // error after collection. Keep malformed entries distinct here.
        effective.push_back(tool);
        return;
    }

    const common_json comparable = canonical_json_value(tool);
    const auto        existing   = definitions.find(identity);
    if (existing == definitions.end()) {
        definitions.emplace(identity, effective_tool_definition{ comparable, effective.size() });
        effective.push_back(tool);
        return;
    }
    if (optional_string_field(tool, "type") == "namespace" &&
        optional_string_field(effective.at(existing->second.index), "type") == "namespace") {
        merge_namespace_declaration(effective[existing->second.index], tool, prefix);
        existing->second.comparable = canonical_json_value(effective.at(existing->second.index));
        return;
    }
    if (existing->second.comparable != comparable) {
        throw invalid_request_field(prefix, "invalid_value",
                                    "Conflicting tool declarations were supplied for '" + identity + "'.");
    }
    // A stored Responses Lite lineage may contain the same complete tool
    // snapshot more than once. Exact semantic duplicates are one declaration,
    // not a reason to grow the model-visible schema on each continuation.
}

common_json collect_effective_tools(const common_json & request, const common_json & input) {
    common_json                                      effective = common_json::array();
    std::map<std::string, effective_tool_definition> definitions;

    if (request.contains("tools") && request.at("tools").is_array()) {
        std::size_t index = 0;
        for (const common_json & tool : request.at("tools")) {
            append_effective_tool(effective, definitions, tool, "tools[" + std::to_string(index) + "]");
            ++index;
        }
    }

    std::size_t item_index = 0;
    for (const common_json & item : input) {
        if (!item.is_object() || optional_string_field(item, "type") != "additional_tools") {
            ++item_index;
            continue;
        }
        const std::string prefix = "input[" + std::to_string(item_index) + "]";
        if (!item.contains("role")) {
            throw invalid_request_field(prefix + ".role", "missing_required_parameter",
                                        "Missing required parameter: '" + prefix + ".role'.");
        }
        if (!item.at("role").is_string()) {
            throw invalid_request_field(prefix + ".role", "invalid_type",
                                        "Invalid type for '" + prefix + ".role': expected a string.");
        }
        if (item.at("role").get<std::string>() != "developer") {
            throw invalid_request_field(prefix + ".role", "invalid_value",
                                        "Invalid value for '" + prefix + ".role': expected 'developer'.");
        }
        if (!item.contains("tools")) {
            throw invalid_request_field(prefix + ".tools", "missing_required_parameter",
                                        "Missing required parameter: '" + prefix + ".tools'.");
        }
        if (!item.at("tools").is_array()) {
            throw invalid_request_field(prefix + ".tools", "invalid_type",
                                        "Invalid type for '" + prefix + ".tools': expected an array.");
        }

        // Reuse the ordinary declaration validator for each positional item so
        // nested schema and within-item collision behavior cannot drift.
        common_json item_request = {
            { "tools", item.at("tools") }
        };
        (void) validate_tools(item_request);
        std::size_t tool_index = 0;
        for (const common_json & tool : item.at("tools")) {
            append_effective_tool(effective, definitions, tool, prefix + ".tools[" + std::to_string(tool_index) + "]");
            ++tool_index;
        }
        ++item_index;
    }

    item_index = 0;
    for (const common_json & item : input) {
        if (item.is_object() && optional_string_field(item, "type") == "tool_search_output" && item.contains("tools") &&
            item.at("tools").is_array()) {
            std::size_t tool_index = 0;
            for (const common_json & tool : item.at("tools")) {
                append_effective_tool(
                    effective, definitions, tool,
                    "input[" + std::to_string(item_index) + "].tools[" + std::to_string(tool_index) + "]");
                ++tool_index;
            }
        }
        ++item_index;
    }

    common_json effective_request = {
        { "tools", effective }
    };
    (void) validate_tools(effective_request);
    return effective;
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
        if (value == "required" && !declared.has_required_choice_target()) {
            throw invalid_request_field("tool_choice", "invalid_value",
                                        "Invalid value for 'tool_choice': no directly callable tools were declared.");
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
    if (type == "tool_search") {
        throw invalid_request_field("tool_choice.type", "invalid_value",
                                    "Client tool_search cannot be selected directly; use tool_choice 'auto'.");
    }
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
        if (!reasoning.at("context").is_string()) {
            throw invalid_request_field("reasoning.context", "invalid_type",
                                        "Invalid type for 'reasoning.context': expected a string or null.");
        }
        if (reasoning.at("context").get<std::string>() != "all_turns") {
            throw invalid_request_field("reasoning.context", "invalid_value",
                                        "Invalid value for 'reasoning.context': expected 'all_turns'.");
        }
        // Responses Lite moves the base instructions and tool declarations
        // into replayable input items. This route already materializes the
        // complete lineage for each inference, so all_turns is truthful and
        // requires no separate opaque reasoning store.
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
    if (request.contains("parallel_tool_calls") && !request.at("parallel_tool_calls").is_boolean()) {
        throw invalid_request_field("parallel_tool_calls", "invalid_type",
                                    "Invalid type for 'parallel_tool_calls': expected a boolean or null.");
    }
    validate_metadata(request);
    validate_client_metadata(request);
    (void) validate_tools(request);
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
    for (const char * field : { "return_progress", "timings_per_token" }) {
        if (!request.contains(field) || request.at(field).is_null()) {
            continue;
        }
        if (!request.at(field).is_boolean()) {
            throw invalid_request_field(field, "invalid_type",
                                        std::string("Invalid type for '") + field + "': expected a boolean or null.");
        }
        if (request.at(field).get<bool>()) {
            throw unsupported_request_field(field, "llama-server telemetry extensions are not exposed by Responses");
        }
    }
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

std::string selected_tool_identity(const std::string & namespace_name, const std::string & name) {
    return namespace_name + '\n' + name;
}

common_json comparable_selected_tool(common_json         tool,
                                     const std::string & namespace_name,
                                     const std::string & namespace_description = {}) {
    tool.erase("defer_loading");
    if (!namespace_name.empty()) {
        tool["namespace"] = namespace_name;
    }
    if (!namespace_description.empty()) {
        tool["_llama_namespace_description"] = namespace_description;
    }
    return canonical_json_value(tool);
}

void remember_selected_tool(const common_json &                  tool,
                            const std::string &                  namespace_name,
                            const std::string &                  namespace_description,
                            const std::string &                  prefix,
                            std::map<std::string, common_json> & selected) {
    const std::string name       = optional_string_field(tool, "name");
    const std::string identity   = selected_tool_identity(namespace_name, name);
    common_json       comparable = comparable_selected_tool(tool, namespace_name, namespace_description);
    const auto        existing   = selected.find(identity);
    if (existing == selected.end()) {
        selected.emplace(identity, std::move(comparable));
        return;
    }
    if (existing->second != comparable) {
        throw invalid_request_field(prefix, "invalid_value",
                                    "Conflicting definitions were supplied for selected tool '" +
                                        (namespace_name.empty() ? name : namespace_name + "." + name) + "'.");
    }
}

void validate_selected_tool(const common_json &                  tool,
                            const std::string &                  prefix,
                            std::map<std::string, common_json> & selected) {
    if (!tool.is_object()) {
        throw invalid_request_field(prefix, "invalid_type", "Invalid type for '" + prefix + "': expected an object.");
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
    if (type == "function" || type == "custom") {
        if (type == "function") {
            validate_function_tool(tool, prefix, 128U);
        } else {
            validate_custom_tool(tool, prefix, 128U);
        }
        validate_optional_tool_field(tool, "namespace", prefix, &common_json::is_string, "a string or null");
        const std::string namespace_name = optional_string_field(tool, "namespace");
        remember_selected_tool(tool, namespace_name, {}, prefix, selected);
        return;
    }
    if (type != "namespace") {
        throw invalid_request_field(
            prefix + ".type", "invalid_value",
            "Invalid value for '" + prefix + ".type': expected 'function', 'custom', or 'namespace'.");
    }

    const std::string namespace_name        = require_tool_name(tool, prefix + ".name");
    const std::string namespace_description = optional_string_field(tool, "description");
    validate_optional_tool_field(tool, "description", prefix, &common_json::is_string, "a string or null");
    if (!tool.contains("tools")) {
        throw invalid_request_field(prefix + ".tools", "missing_required_parameter",
                                    "Missing required parameter: '" + prefix + ".tools'.");
    }
    if (!tool.at("tools").is_array()) {
        throw invalid_request_field(prefix + ".tools", "invalid_type",
                                    "Invalid type for '" + prefix + ".tools': expected an array.");
    }
    std::size_t nested_index = 0;
    for (const common_json & nested : tool.at("tools")) {
        const std::string nested_prefix = prefix + ".tools[" + std::to_string(nested_index) + "]";
        if (!nested.is_object()) {
            throw invalid_request_field(nested_prefix, "invalid_type",
                                        "Invalid type for '" + nested_prefix + "': expected an object.");
        }
        const std::string nested_type = optional_string_field(nested, "type");
        if (nested_type == "function") {
            validate_function_tool(nested, nested_prefix, 128U);
        } else if (nested_type == "custom") {
            validate_custom_tool(nested, nested_prefix, 128U);
        } else {
            throw invalid_request_field(nested_prefix + ".type", "invalid_value",
                                        "Invalid selected namespace member type; expected 'function' or 'custom'.");
        }
        remember_selected_tool(nested, namespace_name, namespace_description, nested_prefix, selected);
        ++nested_index;
    }
}

void validate_tool_search_lineage(const common_json & items) {
    std::set<std::string>              calls;
    std::set<std::string>              outputs;
    std::map<std::string, common_json> selected;

    std::size_t index = 0;
    for (const common_json & item : items) {
        if (!item.is_object()) {
            ++index;
            continue;
        }
        const std::string type   = item.value("type", std::string());
        const std::string prefix = "input[" + std::to_string(index) + "]";
        if (type == "tool_search_call") {
            const std::string call = optional_string_field(item, "call_id");
            if (call.empty()) {
                throw invalid_request_field(prefix + ".call_id", "missing_required_parameter",
                                            "Tool search call requires a non-empty call_id.");
            }
            if (!item.contains("execution")) {
                throw invalid_request_field(prefix + ".execution", "missing_required_parameter",
                                            "Tool search call requires execution 'client'.");
            }
            if (!item.at("execution").is_string()) {
                throw invalid_request_field(prefix + ".execution", "invalid_type",
                                            "Tool search call execution must be a string.");
            }
            if (item.at("execution") != "client") {
                throw unsupported_request_field(prefix + ".execution", "Hosted tool search lineage is not available");
            }
            if (!item.contains("arguments") || !item.at("arguments").is_object()) {
                throw invalid_request_field(prefix + ".arguments", "invalid_type",
                                            "Tool search call arguments must be an object.");
            }
            if (!calls.insert(call).second) {
                throw invalid_request_field(prefix + ".call_id", "invalid_value",
                                            "Tool search call_id values must be unique.");
            }
        } else if (type == "tool_search_output") {
            const std::string call = optional_string_field(item, "call_id");
            if (call.empty()) {
                throw invalid_request_field(prefix + ".call_id", "missing_required_parameter",
                                            "Tool search output requires a non-empty call_id.");
            }
            if (!item.contains("execution")) {
                throw invalid_request_field(prefix + ".execution", "missing_required_parameter",
                                            "Tool search output requires execution 'client'.");
            }
            if (!item.at("execution").is_string()) {
                throw invalid_request_field(prefix + ".execution", "invalid_type",
                                            "Tool search output execution must be a string.");
            }
            if (item.at("execution") != "client") {
                throw unsupported_request_field(prefix + ".execution", "Hosted tool search lineage is not available");
            }
            if (item.contains("status") && !item.at("status").is_null()) {
                if (!item.at("status").is_string()) {
                    throw invalid_request_field(prefix + ".status", "invalid_type",
                                                "Tool search output status must be a string.");
                }
                const std::string status = item.at("status").get<std::string>();
                if (status != "in_progress" && status != "completed" && status != "incomplete") {
                    throw invalid_request_field(prefix + ".status", "invalid_value",
                                                "Invalid tool search output status.");
                }
            }
            if (!item.contains("tools")) {
                throw invalid_request_field(prefix + ".tools", "missing_required_parameter",
                                            "Tool search output requires a tools array.");
            }
            if (!item.at("tools").is_array()) {
                throw invalid_request_field(prefix + ".tools", "invalid_type",
                                            "Tool search output tools must be an array.");
            }
            if (calls.find(call) == calls.end()) {
                throw invalid_request_field("input", "invalid_value",
                                            "No preceding tool search call found for call_id " + call + ".");
            }
            if (!outputs.insert(call).second) {
                throw invalid_request_field(prefix + ".call_id", "invalid_value",
                                            "Only one output may answer a tool search call.");
            }
            std::size_t tool_index = 0;
            for (const common_json & tool : item.at("tools")) {
                validate_selected_tool(tool, prefix + ".tools[" + std::to_string(tool_index) + "]", selected);
                ++tool_index;
            }
        }
        ++index;
    }

    for (const std::string & call : calls) {
        if (outputs.find(call) == outputs.end()) {
            throw invalid_request_field("input", "invalid_value",
                                        "No tool output found for tool search call " + call + ".");
        }
    }
}

std::optional<std::uint64_t> parse_starting_after(const server_http_req & request) {
    const auto found = request.params.find("starting_after");
    if (found == request.params.end()) {
        return std::nullopt;
    }
    if (found->second.empty() || !std::all_of(found->second.begin(), found->second.end(),
                                              [](unsigned char character) { return std::isdigit(character) != 0; })) {
        throw invalid_request_field("starting_after", "invalid_parameter",
                                    "Invalid starting_after event sequence number");
    }
    try {
        std::size_t         parsed = 0;
        const std::uint64_t value  = std::stoull(found->second, &parsed);
        if (parsed != found->second.size()) {
            throw std::invalid_argument("trailing cursor data");
        }
        return value;
    } catch (const std::exception &) {
        throw invalid_request_field("starting_after", "invalid_parameter",
                                    "Invalid starting_after event sequence number");
    }
}

std::uint64_t event_sequence_number(const common_json & event) {
    if (!event.is_object() || !event.contains("sequence_number") || !event.at("sequence_number").is_number_integer()) {
        throw std::runtime_error("Stored Responses event has no sequence number");
    }
    return event.at("sequence_number").get<std::uint64_t>();
}

std::string event_sse_frames(const std::vector<common_json> & events) {
    std::string output;
    for (const common_json & event : events) {
        if (!event.is_object() || !event.contains("type") || !event.at("type").is_string()) {
            throw std::runtime_error("Stored Responses event has no type");
        }
        output += "event: ";
        output += event.at("type").get<std::string>();
        output += "\ndata: ";
        output += event.dump();
        output += "\n\n";
    }
    return output;
}

std::string stream_error_sse_frame(const std::optional<std::uint64_t> & cursor,
                                   const std::string &                  code,
                                   const std::string &                  message) {
    std::uint64_t sequence_number = 0;
    if (cursor) {
        sequence_number = *cursor == std::numeric_limits<std::uint64_t>::max() ? *cursor : *cursor + 1U;
    }
    const common_json event = {
        { "type",            "error"         },
        { "sequence_number", sequence_number },
        { "code",            code            },
        { "message",         message         },
        { "param",           nullptr         },
    };
    return "event: error\ndata: " + event.dump() + "\n\n";
}

struct response_journal_cursor {
    response_store &                               store;
    response_id                                    id;
    std::optional<std::uint64_t>                   cursor;
    std::function<bool()>                          should_stop;
    std::shared_ptr<native_server_generation_sink> active_sink;

    bool next(std::string & output) {
        try {
            while (!should_stop()) {
                const auto page = store.events_after(id, cursor);
                if (!page) {
                    if (active_sink && active_sink->storage_failed()) {
                        output = stream_error_sse_frame(cursor, "response_store_error", active_sink->storage_error());
                    } else {
                        output.clear();
                    }
                    return false;
                }
                if (!page->events.empty()) {
                    output = event_sse_frames(page->events);
                    cursor = event_sequence_number(page->events.back());
                    return !response_status_is_terminal(page->head.status) || page->head.next_sequence_number == 0 ||
                           *cursor < page->head.next_sequence_number - 1U;
                }
                if (response_status_is_terminal(page->head.status)) {
                    output.clear();
                    return false;
                }
                store.wait_for_event_change(page->change_epoch, 200);
            }
            output.clear();
            return false;
        } catch (const std::exception & error) {
            output = stream_error_sse_frame(cursor, "response_store_error", error.what());
            return false;
        } catch (...) {
            output = stream_error_sse_frame(cursor, "server_error", "Response event streaming failed");
            return false;
        }
    }
};

server_http_res_ptr journal_stream_response(response_store &                               store,
                                            const response_id &                            id,
                                            std::optional<std::uint64_t>                   starting_after,
                                            const std::function<bool()> &                  should_stop,
                                            std::shared_ptr<native_server_generation_sink> active_sink = nullptr) {
    auto cursor = std::make_shared<response_journal_cursor>(
        response_journal_cursor{ store, id, starting_after, should_stop, std::move(active_sink) });
    auto response          = std::make_unique<server_http_res>();
    response->content_type = "text/event-stream";
    response->next         = [cursor = std::move(cursor)](std::string & output) {
        return cursor->next(output);
    };
    return response;
}

class active_response_registry {
  public:
    void add(const std::shared_ptr<native_server_generation_sink> & sink) {
        std::lock_guard<std::mutex> lock(mutex);
        erase_expired_unlocked();
        active[sink->id().str()] = sink;
    }

    std::shared_ptr<native_server_generation_sink> find(const response_id & id) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto                  found = active.find(id.str());
        if (found == active.end()) {
            return nullptr;
        }
        auto sink = found->second.lock();
        if (!sink) {
            active.erase(found);
        }
        return sink;
    }

    void remove(const std::shared_ptr<native_server_generation_sink> & sink) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto                  found = active.find(sink->id().str());
        if (found == active.end()) {
            return;
        }
        const auto registered = found->second.lock();
        if (!registered || registered == sink) {
            active.erase(found);
        }
    }

    void request_cancel_all() {
        std::vector<std::shared_ptr<native_server_generation_sink>> sinks;
        {
            std::lock_guard<std::mutex> lock(mutex);
            erase_expired_unlocked();
            sinks.reserve(active.size());
            for (const auto & entry : active) {
                if (auto sink = entry.second.lock()) {
                    sinks.push_back(std::move(sink));
                }
            }
        }
        for (const auto & sink : sinks) {
            if (!sink->terminal()) {
                sink->request_cancel();
            }
        }
    }

  private:
    void erase_expired_unlocked() {
        for (auto item = active.begin(); item != active.end();) {
            if (item->second.expired()) {
                item = active.erase(item);
            } else {
                ++item;
            }
        }
    }

    std::mutex                                                                    mutex;
    std::unordered_map<std::string, std::weak_ptr<native_server_generation_sink>> active;
};

bool header_name_equals(const std::string & lhs, const char * rhs) {
    const std::size_t rhs_size = std::char_traits<char>::length(rhs);
    return lhs.size() == rhs_size &&
           std::equal(lhs.begin(), lhs.end(), rhs, [](unsigned char left, unsigned char right) {
               return std::tolower(left) == std::tolower(right);
           });
}

std::map<std::string, std::string> background_worker_headers(const std::map<std::string, std::string> & source) {
    std::map<std::string, std::string> result;
    for (const auto & header : source) {
        if (!header_name_equals(header.first, "X-Conversation-Id")) {
            result.insert(header);
        }
    }
    return result;
}

class background_response_job final {
  public:
    background_response_job(server_generation_service                      service,
                            const server_http_req &                        source_request,
                            server_generation_input                        generation,
                            std::shared_ptr<native_server_generation_sink> sink,
                            active_response_registry &                     active_responses) :
        service(std::move(service)),
        sink(std::move(sink)),
        should_stop([this] { return stop_requested.load() || this->sink->cancel_requested(); }),
        request{
            source_request.params, background_worker_headers(source_request.headers),
            source_request.path,   source_request.query_string,
            source_request.body,   source_request.files,
            should_stop,
        },
        generation(std::move(generation)),
        active_responses(active_responses) {
        // The worker owns this private transport. For a streaming background
        // response it drains model deltas into the durable journal; the public
        // HTTP subscriber is a separate reader and can disconnect harmlessly.
    }

    ~background_response_job() {
        request_stop();
        join();
    }

    background_response_job(const background_response_job &)             = delete;
    background_response_job & operator=(const background_response_job &) = delete;

    void start() {
        worker = std::thread([this] { run(); });
    }

    void request_stop() noexcept {
        stop_requested.store(true);
        sink->request_cancel();
    }

    bool finished() const noexcept { return done.load(std::memory_order_acquire); }

    void join() noexcept {
        if (worker.joinable()) {
            worker.join();
        }
    }

  private:
    void terminalize_after_failure(const std::string & message) noexcept {
        if (sink->terminal()) {
            return;
        }
        try {
            if (stop_requested.load() || sink->cancel_requested()) {
                sink->accept(server_generation_cancelled{});
            } else {
                sink->accept(server_generation_failed{
                    { "server_error", message, "" },
                    std::nullopt,
                });
            }
        } catch (const std::exception & error) {
            LOG_ERR("background Responses job %s could not record its terminal state: %s\n", sink->id().str().c_str(),
                    error.what());
        }
    }

    void run() noexcept {
        try {
            server_http_res_ptr response = service.generate(request, generation, sink);
            if (!response) {
                terminalize_after_failure("Background generation returned no response");
            } else {
                if (response->is_stream()) {
                    std::string ignored;
                    while (response->next(ignored)) {
                        ignored.clear();
                    }
                } else if (response->status < 200 || response->status >= 300) {
                    terminalize_after_failure("Background generation failed before producing a terminal response");
                }
                response->on_complete();
            }
        } catch (const std::exception & error) {
            terminalize_after_failure(error.what());
        } catch (...) {
            terminalize_after_failure("Background generation failed with an unknown error");
        }

        if (!sink->terminal()) {
            terminalize_after_failure("Background generation ended without a terminal response");
        }
        active_responses.remove(sink);
        done.store(true, std::memory_order_release);
    }

    server_generation_service                      service;
    std::shared_ptr<native_server_generation_sink> sink;
    std::atomic_bool                               stop_requested{ false };
    std::function<bool()>                          should_stop;
    server_http_req                                request;
    server_generation_input                        generation;
    active_response_registry &                     active_responses;
    std::thread                                    worker;
    std::atomic_bool                               done{ false };
};

class background_response_jobs final {
  public:
    background_response_jobs(server_generation_service service, active_response_registry & active_responses) :
        service(std::move(service)),
        active_responses(active_responses) {}

    ~background_response_jobs() { shutdown(); }

    void start(const server_http_req &                                request,
               server_generation_input                                generation,
               const std::shared_ptr<native_server_generation_sink> & sink) {
        reap();
        auto job =
            std::make_shared<background_response_job>(service, request, std::move(generation), sink, active_responses);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) {
                throw std::runtime_error("Responses background executor is stopping");
            }
            jobs.emplace(sink->id().str(), job);
            try {
                job->start();
            } catch (...) {
                jobs.erase(sink->id().str());
                throw;
            }
        }
    }

    void reap() noexcept {
        std::vector<std::shared_ptr<background_response_job>> finished;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto item = jobs.begin(); item != jobs.end();) {
                if (item->second->finished()) {
                    finished.push_back(std::move(item->second));
                    item = jobs.erase(item);
                } else {
                    ++item;
                }
            }
        }
        for (const auto & job : finished) {
            job->join();
        }
    }

    void shutdown() noexcept {
        std::vector<std::shared_ptr<background_response_job>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            pending.reserve(jobs.size());
            for (auto & entry : jobs) {
                pending.push_back(std::move(entry.second));
            }
            jobs.clear();
        }
        for (const auto & job : pending) {
            job->request_stop();
        }
        for (const auto & job : pending) {
            job->join();
        }
    }

  private:
    server_generation_service                                                 service;
    active_response_registry &                                                active_responses;
    std::mutex                                                                mutex;
    std::unordered_map<std::string, std::shared_ptr<background_response_job>> jobs;
    bool                                                                      stopping = false;
};

class responses_routes_impl final {
  public:
    explicit responses_routes_impl(server_generation_service service) :
        service(std::move(service)),
        store(open_response_store()),
        resources(*store),
        background_jobs(this->service, active_responses) {
        if (!this->service.generate || !this->service.count_input_tokens) {
            throw std::invalid_argument("llama-responses requires typed generation and token-counting services");
        }
    }

    static server_responses_routes routes(const std::shared_ptr<responses_routes_impl> & self) {
        server_responses_routes result;
        result.owner    = self;
        result.shutdown = [self] {
            self->shutdown();
        };
        result.create = [self](const server_http_req & request) {
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
        result.cancel = [self](const server_http_req & request) {
            return self->cancel(request);
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
    static std::unique_ptr<response_store> open_response_store() {
        auto              result      = std::make_unique<sqlite_response_store>(default_sqlite_response_store_path());
        const std::size_t interrupted = result->fail_interrupted_responses();
        if (interrupted != 0U) {
            LOG_WRN("marked %zu interrupted Responses request(s) failed after restart\n", interrupted);
        }
        return result;
    }

    struct prepared_request {
        server_generation_input generation;
        common_json             original    = common_json::object();
        common_json             input_items = common_json::array();
    };

    server_generation_service       service;
    std::unique_ptr<response_store> store;
    response_resource_service       resources;
    active_response_registry        active_responses;
    background_response_jobs        background_jobs;
    // The registry establishes the native C++ strategy boundary now. Its
    // default providers are deliberately unavailable until Phase 4 adapters
    // are configured.
    hosted_tool_registry            hosted_tools;

    void shutdown() noexcept {
        active_responses.request_cancel_all();
        background_jobs.shutdown();
    }

    static item_id make_input_id(std::size_t /*index*/, const common_json & item) {
        std::string prefix = "item_";
        if (item.is_object()) {
            const std::string type = item.value("type", std::string());
            if (type == "message") {
                prefix = "msg_";
            } else if (type == "additional_tools") {
                prefix = "at_";
            }
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
        // Public selection requires a visible target, while the store's graph
        // materializer may traverse tombstoned ancestors which still belong to
        // this already-created lineage.
        const auto parent = store->find(previous);
        if (!parent) {
            throw std::out_of_range(previous.str());
        }
        if (!response_status_is_terminal(parent->status)) {
            throw invalid_request_field("previous_response_id", "response_not_complete",
                                        "Response '" + previous.str() + "' has not reached a terminal state.");
        }
        const auto context = store->materialize_continuation_context(previous);
        if (!context) {
            throw std::out_of_range(previous.str());
        }
        for (const common_json & item : *context) {
            expanded.push_back(item);
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
        validate_tool_search_lineage(expanded);

        common_json effective_tools_request = {
            { "tools", collect_effective_tools(original, expanded) },
        };
        const declared_tools effective_tools = validate_tools(effective_tools_request);
        validate_tool_choice(original, effective_tools);
        // `additional_tools` is an ordered input representation, but the
        // response envelope still reports the active declaration set in its
        // ordinary `tools` field. Persist that projection with the response so
        // retrieval matches the create result without reparsing lineage.
        original["tools"] = effective_tools_request.at("tools");

        common_json generation_request = original;
        generation_request["input"]    = expanded;
        generation_request["tools"]    = std::move(effective_tools_request["tools"]);
        if (generation_request.contains("max_output_tokens") &&
            generation_request.at("max_output_tokens").is_number_integer() &&
            !json_integer_is_negative(generation_request.at("max_output_tokens")) &&
            generation_request.at("max_output_tokens").get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            // The shared inference runtime uses an int token budget. Values
            // above that are observationally unbounded for a local context;
            // retain the caller's 64-bit value in `original` for the envelope.
            generation_request["max_output_tokens"] = std::numeric_limits<int>::max();
        }
        if (normalized.contains("instructions")) {
            generation_request["instructions"] = normalized.at("instructions");
        }
        server_generation_input generation = lower_responses_generation_input(generation_request);
        return {
            std::move(generation),
            std::move(original),
            std::move(current_items),
        };
    }

    server_http_res_ptr create(const server_http_req & request) {
        background_jobs.reap();
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

        if (prepared.original.value("background", false)) {
            return create_background(request, std::move(prepared));
        }
        return create_native(request, prepared);
    }

    static generation_response_context native_context(const prepared_request & prepared) {
        generation_response_context context;
        context.model       = prepared.original.value("model", std::string());
        context.request     = prepared.original;
        context.input_items = prepared.input_items;
        context.created_at  = static_cast<std::uint64_t>(std::time(nullptr));
        if (prepared.original.contains("previous_response_id") &&
            prepared.original.at("previous_response_id").is_string()) {
            const std::string previous = prepared.original.at("previous_response_id").get<std::string>();
            if (!previous.empty()) {
                context.previous_response = response_id(previous);
            }
        }
        return context;
    }

    server_http_res_ptr create_native(const server_http_req & request, const prepared_request & prepared) {
        const bool       stream  = prepared.original.value("stream", false);
        response_store * storage = request_store_enabled(prepared.original) ? store.get() : nullptr;
        auto sink = std::make_shared<native_server_generation_sink>(native_context(prepared), random_id_suffix(),
                                                                    stream, storage, prepared.generation.tool_metadata);
        active_responses.add(sink);

        server_http_res_ptr response;
        try {
            response = service.generate(request, prepared.generation, sink);
        } catch (const std::invalid_argument & error) {
            active_responses.remove(sink);
            sink->discard_persisted_state();
            return api_error(400, error.what(), "invalid_request");
        } catch (const std::exception & error) {
            active_responses.remove(sink);
            sink->discard_persisted_state();
            if (sink->storage_failed()) {
                return api_error(500, sink->storage_error(), "response_store_error");
            }
            return api_error(500, error.what(), "server_error");
        }
        if (!response) {
            active_responses.remove(sink);
            sink->discard_persisted_state();
            return api_error(500, "Responses generation returned no response", "server_error");
        }
        if (sink->storage_failed() && !response->is_stream()) {
            active_responses.remove(sink);
            return api_error(500, sink->storage_error(), "response_store_error");
        }
        if (response->status < 200 || response->status >= 300) {
            active_responses.remove(sink);
            sink->discard_persisted_state();
            return response;
        }
        if (sink->terminal()) {
            active_responses.remove(sink);
        }
        return response;
    }

    server_http_res_ptr create_background(const server_http_req & request, prepared_request prepared) {
        // OpenAI retains background resources even when `store` is false so
        // they can be polled. SQLite is our process-independent resource
        // backing; expiry policy remains deliberately deferred.
        const bool stream                                  = prepared.original.value("stream", false);
        prepared.generation.inference_parameters["stream"] = stream;
        // The worker's llama-server stream is a private inference transport.
        // Public background SSE is rendered from the durable journal below,
        // so asking the sink to also build transport bytes would serialize
        // every event once only to discard those bytes in the worker.
        auto sink =
            std::make_shared<native_server_generation_sink>(native_context(prepared), random_id_suffix(), false,
                                                            store.get(), prepared.generation.tool_metadata, stream);
        try {
            // Allocate, render, and durably checkpoint the response before the
            // HTTP handler returns. `in_progress` is an allowed initial
            // background state; exact queued scheduling is not observable
            // enough to justify a second scheduler state machine here.
            sink->accept(server_generation_started{});
            if (sink->storage_failed()) {
                throw std::runtime_error(sink->storage_error());
            }
            active_responses.add(sink);
            background_jobs.start(request, std::move(prepared.generation), sink);
            return stream ? journal_stream_response(*store, sink->id(), std::nullopt, request.should_stop, sink) :
                            json_response(sink->snapshot());
        } catch (const std::exception & error) {
            active_responses.remove(sink);
            sink->discard_persisted_state();
            if (sink->storage_failed()) {
                return api_error(500, sink->storage_error(), "response_store_error");
            }
            return api_error(500, error.what(), "server_error");
        }
    }

    server_http_res_ptr input_tokens(const server_http_req & request) {
        background_jobs.reap();
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
        try {
            return json_response({
                { "object",       "response.input_tokens"                         },
                { "input_tokens", service.count_input_tokens(prepared.generation) },
            });
        } catch (const std::invalid_argument & error) {
            return api_error(400, error.what(), "invalid_request");
        } catch (const std::exception & error) {
            return api_error(500, error.what(), "server_error");
        }
    }

    server_http_res_ptr retrieve(const server_http_req & request) {
        background_jobs.reap();
        const auto        stream_parameter = request.params.find("stream");
        const std::string stream = stream_parameter == request.params.end() ? std::string() : stream_parameter->second;
        if (stream_parameter != request.params.end() && stream != "false" && stream != "true") {
            return api_error(400, "Invalid retrieve stream parameter", "invalid_parameter", std::string("stream"));
        }
        std::optional<std::uint64_t> starting_after;
        try {
            starting_after = parse_starting_after(request);
        } catch (const invalid_request_field & error) {
            return api_error(400, error.what(), error.code, error.field);
        }
        if (starting_after && stream != "true") {
            return api_error(400, "starting_after requires stream=true", "invalid_parameter",
                             std::string("starting_after"));
        }
        const auto        include_obfuscation_parameter = request.params.find("include_obfuscation");
        const std::string include_obfuscation           = include_obfuscation_parameter == request.params.end() ?
                                                              std::string() :
                                                              include_obfuscation_parameter->second;
        if (include_obfuscation_parameter != request.params.end() && include_obfuscation != "false" &&
            include_obfuscation != "true") {
            return api_error(400, "Invalid include_obfuscation parameter", "invalid_parameter",
                             std::string("include_obfuscation"));
        }
        if (include_obfuscation == "true") {
            return api_error(501, "Stream obfuscation is not available", "not_supported",
                             std::string("include_obfuscation"));
        }
        static const std::set<std::string> supported_includes = {
            "code_interpreter_call.outputs",  "computer_call_output.output.image_url",
            "file_search_call.results",       "message.input_image.image_url",
            "message.output_text.logprobs",   "reasoning.encrypted_content",
            "web_search_call.action.sources", "web_search_call.results",
        };
        const std::vector<std::string> includes = query_array_values(request, "include");
        for (std::size_t index = 0; index < includes.size(); ++index) {
            if (supported_includes.find(includes[index]) == supported_includes.end()) {
                return api_error(400,
                                 "Invalid value for include[" + std::to_string(index) + "]: '" + includes[index] + "'.",
                                 "invalid_value", "include[" + std::to_string(index) + "]");
            }
            if (includes[index] == "reasoning.encrypted_content") {
                return api_error(400, "Encrypted content cannot be requested for persisted responses.", "",
                                 std::string("include"));
            }
        }
        const response_id id(response_id_param(request));
        if (stream == "true") {
            const auto page = store->events_after(id);
            if (!page) {
                return json_response(render_response_not_found(id), 404);
            }
            if (!page->head.event_journal) {
                return api_error(400, "Response '" + id.str() + "' was not created as a background stream.",
                                 "invalid_parameter", std::string("stream"));
            }
            return journal_stream_response(*store, id, starting_after, request.should_stop, active_responses.find(id));
        }
        if (auto sink = active_responses.find(id)) {
            if (sink->storage_failed()) {
                return api_error(500, sink->storage_error(), "response_store_error");
            }
            return json_response(sink->snapshot());
        }
        const resource_result result = resources.retrieve(id);
        return json_response(result.body, status_for(result.kind));
    }

    server_http_res_ptr erase(const server_http_req & request) {
        background_jobs.reap();
        const response_id id(response_id_param(request));
        if (auto sink = active_responses.find(id)) {
            if (!sink->terminal()) {
                return api_error(409, "Response '" + id.str() + "' is still active and cannot be deleted.",
                                 "response_active", std::string("response_id"));
            }
            active_responses.remove(sink);
        }
        const resource_result result = resources.erase(id);
        return json_response(result.body, status_for(result.kind));
    }

    server_http_res_ptr cancel(const server_http_req & request) {
        background_jobs.reap();
        const response_id id(response_id_param(request));
        auto              sink = active_responses.find(id);
        if (!sink) {
            const resource_result stored = resources.retrieve(id);
            if (stored.kind == resource_result_kind::not_found) {
                return json_response(stored.body, 404);
            }
            const std::string status = stored.body.value("status", std::string());
            if (status == "cancelled") {
                return json_response(stored.body);
            }
            if (status == "completed" || status == "incomplete" || status == "failed") {
                return api_error(400, "Response '" + id.str() + "' has already reached status '" + status + "'.",
                                 "response_not_cancellable", std::string("response_id"));
            }
            return api_error(409, "Response '" + id.str() + "' is persisted but has no active worker.",
                             "response_not_active", std::string("response_id"));
        }

        if (sink->terminal()) {
            active_responses.remove(sink);
            const response_state state = sink->state();
            if (state.status == response_status::cancelled) {
                return json_response(render_response(state));
            }
            return api_error(400,
                             "Response '" + id.str() + "' has already reached status '" +
                                 std::string(response_status_name(state.status)) + "'.",
                             "response_not_cancellable", std::string("response_id"));
        }

        sink->request_cancel();
        static constexpr std::uint64_t cancellation_timeout_ms = 5000;
        if (!sink->wait_for_terminal(cancellation_timeout_ms)) {
            return api_error(409, "Cancellation for response '" + id.str() + "' is still pending.",
                             "response_cancel_pending", std::string("response_id"));
        }
        active_responses.remove(sink);
        if (sink->storage_failed()) {
            return api_error(500, sink->storage_error(), "response_store_error");
        }
        const response_state state = sink->state();
        if (state.status != response_status::cancelled) {
            return api_error(400, "Response '" + id.str() + "' reached a terminal state before cancellation.",
                             "response_not_cancellable", std::string("response_id"));
        }
        return json_response(render_response(state));
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
    return [](server_generation_service service) {
        auto implementation = std::make_shared<responses_routes_impl>(std::move(service));
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
