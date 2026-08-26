#include "input-lowering.h"

#include "base64.hpp"
#include "chat.h"
#include "json.h"
#include "server-generation.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llama_responses {
namespace {

constexpr std::size_t MAX_INLINE_MEDIA_BYTES = 32U * 1024U * 1024U;

struct pending_media {
    std::size_t                  content_part_index = 0;
    server_generation_media_kind kind               = server_generation_media_kind::image;
    std::string                  source;
};

struct lowered_message {
    common_chat_msg            message;
    std::vector<pending_media> media;
};

struct lowering_context {
    server_generation_input result;
    std::size_t             inline_media_bytes = 0;
};

std::string string_field(const common_json & object, const char * key, const std::string & fallback = {}) {
    return object.is_object() && object.contains(key) && object.at(key).is_string() ?
               object.at(key).get<std::string>() :
               fallback;
}

std::string sanitize_tool_name(const std::string & name, const std::string & fallback = "tool") {
    std::string result;
    result.reserve(name.size());
    for (const unsigned char character : name) {
        if (std::isalnum(character) != 0 || character == '_' || character == '-') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('_');
        }
    }
    while (!result.empty() && result.front() == '_') {
        result.erase(result.begin());
    }
    if (result.empty()) {
        result = fallback;
    }
    if (result.size() > 64U) {
        result.resize(64U);
    }
    return result;
}

// This mapping is part of the sidecar's persisted model-facing contract. A
// stable model-visible name lets the neutral output adapter restore the public
// namespace without depending on llama-server's stock Responses converter.
std::string namespace_tool_name(const std::string & namespace_name, const std::string & tool_name) {
    const std::string identity = namespace_name + "\n" + tool_name;
    std::uint64_t     hash     = 14695981039346656037ULL;
    for (const unsigned char character : identity) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string           suffix(16U, '0');
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        suffix[suffix.size() - index - 1U] = hex[hash & 0x0fU];
        hash >>= 4U;
    }

    std::string           prefix     = sanitize_tool_name(namespace_name + "_" + tool_name, "namespace_tool");
    constexpr std::size_t max_prefix = 64U - 1U - 16U;
    if (prefix.size() > max_prefix) {
        prefix.resize(max_prefix);
    }
    return prefix + "_" + suffix;
}

void append_text(common_chat_msg & message, std::string text) {
    message.content_parts.push_back({ "text", std::move(text) });
}

void append_recovery(common_chat_msg & message, const std::string & detail) {
    append_text(message, "[responses recovery: " + detail + "]");
}

void append_media(lowered_message & message, server_generation_media_kind kind, std::string source) {
    const std::size_t part_index = message.message.content_parts.size();
    message.message.content_parts.push_back({ "media_marker", {} });
    message.media.push_back({ part_index, kind, std::move(source) });
}

void consume_inline_media(lowering_context & context, const std::string & source) {
    if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0 || source.rfind("file://", 0) == 0) {
        return;
    }
    if (source.size() > MAX_INLINE_MEDIA_BYTES - context.inline_media_bytes) {
        throw std::invalid_argument("inline media exceeds the 32 MiB request limit");
    }
    context.inline_media_bytes += source.size();
}

void append_lowered_message(lowering_context & context, lowered_message message, bool merge_with_previous = false) {
    std::size_t message_index = context.result.chat.messages.size();
    if (merge_with_previous && !context.result.chat.messages.empty()) {
        message_index                 = context.result.chat.messages.size() - 1U;
        common_chat_msg & target      = context.result.chat.messages.back();
        const std::size_t part_offset = target.content_parts.size();
        target.content_parts.insert(target.content_parts.end(),
                                    std::make_move_iterator(message.message.content_parts.begin()),
                                    std::make_move_iterator(message.message.content_parts.end()));
        target.tool_calls.insert(target.tool_calls.end(), std::make_move_iterator(message.message.tool_calls.begin()),
                                 std::make_move_iterator(message.message.tool_calls.end()));
        if (!message.message.reasoning_content.empty()) {
            target.reasoning_content = std::move(message.message.reasoning_content);
        }
        for (pending_media & media : message.media) {
            media.content_part_index += part_offset;
        }
    } else {
        context.result.chat.messages.push_back(std::move(message.message));
    }

    for (pending_media & media : message.media) {
        consume_inline_media(context, media.source);
        context.result.media.push_back({
            message_index,
            media.content_part_index,
            media.kind,
            std::move(media.source),
        });
    }
}

struct data_uri_view {
    std::string mime_type;
    std::string payload;
    bool        base64 = false;
};

data_uri_view parse_data_uri(const std::string & value) {
    if (value.rfind("data:", 0) != 0) {
        return {};
    }
    const std::size_t comma = value.find(',');
    if (comma == std::string::npos) {
        throw std::invalid_argument("invalid input_file data URI");
    }
    std::string   descriptor = value.substr(5U, comma - 5U);
    data_uri_view result;
    result.payload                  = value.substr(comma + 1U);
    const std::string base64_suffix = ";base64";
    if (descriptor.size() >= base64_suffix.size() &&
        descriptor.compare(descriptor.size() - base64_suffix.size(), base64_suffix.size(), base64_suffix) == 0) {
        result.base64 = true;
        descriptor.resize(descriptor.size() - base64_suffix.size());
    }
    result.mime_type = std::move(descriptor);
    return result;
}

std::string decode_text_data_uri(const data_uri_view & uri) {
    if (!uri.base64) {
        return uri.payload;
    }
    try {
        return base64::decode(uri.payload);
    } catch (const std::exception & error) {
        throw std::invalid_argument(std::string("invalid base64 text file: ") + error.what());
    }
}

std::string file_label(const common_json & item) {
    std::string filename = string_field(item, "filename");
    if (!filename.empty()) {
        return filename;
    }
    const std::string file_id = string_field(item, "file_id");
    return file_id.empty() ? "file" : file_id;
}

bool filename_has_suffix(const std::string & filename, std::initializer_list<std::string_view> suffixes) {
    std::string normalized = filename;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return std::any_of(suffixes.begin(), suffixes.end(), [&](std::string_view suffix) {
        return normalized.size() >= suffix.size() &&
               normalized.compare(normalized.size() - suffix.size(), suffix.size(), suffix) == 0;
    });
}

std::string decode_text_file_best_effort(const std::string & data) {
    const bool plausible_base64 =
        !data.empty() && data.size() % 4U == 0U && std::all_of(data.begin(), data.end(), [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '+' || character == '/' || character == '=';
        });
    if (!plausible_base64) {
        return data;
    }
    try {
        std::string decoded         = base64::decode(data);
        const bool  looks_like_text = std::all_of(decoded.begin(), decoded.end(), [](unsigned char character) {
            return character == '\n' || character == '\r' || character == '\t' || character >= 0x20U;
        });
        if (looks_like_text) {
            return decoded;
        }
        return data;
    } catch (const std::exception &) {
        // `file_data` predates the base64-only OpenAI spelling in this fork;
        // an ambiguous value remains decoded caller text for compatibility.
        return data;
    }
}

void append_file(lowered_message & message, const common_json & item) {
    const std::string data = string_field(item, "file_data");
    if (data.empty()) {
        const std::string file_url = string_field(item, "file_url");
        const std::string label    = file_label(item);
        if (!file_url.empty() &&
            (filename_has_suffix(label, { ".avif", ".bmp", ".gif", ".jpeg", ".jpg", ".png", ".webp" }) ||
             filename_has_suffix(file_url, { ".avif", ".bmp", ".gif", ".jpeg", ".jpg", ".png", ".webp" }))) {
            append_text(message.message, "[file: " + label + "]");
            append_media(message, server_generation_media_kind::image, file_url);
            return;
        }
        if (!string_field(item, "file_id").empty() || !file_url.empty()) {
            throw std::invalid_argument("input_file references require a configured file-content provider");
        }
        append_text(message.message, "[file: " + label + " data unavailable]");
        return;
    }

    const std::string   label = file_label(item);
    const data_uri_view uri   = parse_data_uri(data);
    if (uri.mime_type.rfind("image/", 0) == 0) {
        if (!label.empty()) {
            append_text(message.message, "[file: " + label + "]");
        }
        append_media(message, server_generation_media_kind::image, data);
        return;
    }
    if (uri.mime_type.rfind("audio/", 0) == 0) {
        if (!uri.base64) {
            throw std::invalid_argument("audio input_file data must be base64 encoded");
        }
        append_text(message.message, "[file: " + label + "]");
        append_media(message, server_generation_media_kind::audio, uri.payload);
        return;
    }
    if (uri.mime_type.rfind("video/", 0) == 0) {
        if (!uri.base64) {
            throw std::invalid_argument("video input_file data must be base64 encoded");
        }
        append_text(message.message, "[file: " + label + "]");
        append_media(message, server_generation_media_kind::video, uri.payload);
        return;
    }
    if (!uri.mime_type.empty() && uri.mime_type.rfind("text/", 0) != 0 && uri.mime_type != "application/json") {
        throw std::invalid_argument("unsupported input_file media type: " + uri.mime_type);
    }
    if (uri.mime_type.empty() &&
        filename_has_suffix(label, { ".avif", ".bmp", ".gif", ".jpeg", ".jpg", ".png", ".webp" })) {
        append_text(message.message, "[file: " + label + "]");
        append_media(message, server_generation_media_kind::image, data);
        return;
    }
    if (uri.mime_type.empty() && filename_has_suffix(label, { ".mp3", ".ogg", ".wav" })) {
        append_text(message.message, "[file: " + label + "]");
        append_media(message, server_generation_media_kind::audio, data);
        return;
    }
    if (uri.mime_type.empty() && filename_has_suffix(label, { ".avi", ".mkv", ".mov", ".mp4", ".webm" })) {
        append_text(message.message, "[file: " + label + "]");
        append_media(message, server_generation_media_kind::video, data);
        return;
    }
    if (uri.mime_type.empty() && filename_has_suffix(label, { ".pdf" })) {
        throw std::invalid_argument("unsupported input_file media type inferred from " + label);
    }
    std::string text;
    if (!uri.mime_type.empty()) {
        text = decode_text_data_uri(uri);
    } else if (filename_has_suffix(label, { ".c", ".cc", ".cpp", ".csv", ".h", ".hpp", ".html", ".json", ".log", ".md",
                                            ".py", ".rst", ".toml", ".ts", ".txt", ".xml", ".yaml", ".yml" })) {
        text = decode_text_file_best_effort(data);
    } else {
        // Preserve the legacy raw-text extension for callers which already
        // supplied decoded file content without a MIME type or known suffix.
        text = data;
    }
    append_text(message.message, "[file: " + label + "]\n" + text);
}

void append_content_item(lowered_message & message, const common_json & part) {
    if (!part.is_object()) {
        append_text(message.message, part.is_string() ? part.get<std::string>() : part.dump());
        return;
    }
    const std::string type = string_field(part, "type");
    if (type == "input_text" || type == "output_text" || type == "text" || type == "reasoning_text") {
        if (!part.contains("text") || !part.at("text").is_string()) {
            append_recovery(message.message, type + " content item is missing text");
            return;
        }
        append_text(message.message, part.at("text").get<std::string>());
        return;
    }
    if (type == "refusal") {
        const std::string refusal = string_field(part, "refusal");
        if (refusal.empty()) {
            append_recovery(message.message, "refusal content item is missing refusal");
            return;
        }
        append_text(message.message, "[assistant refusal] " + refusal);
        return;
    }
    if (type == "input_image" || type == "computer_screenshot") {
        const std::string source = string_field(part, "image_url");
        if (source.empty()) {
            if (!string_field(part, "file_id").empty()) {
                throw std::invalid_argument(type + " file_id requires a configured file-content provider");
            }
            append_recovery(message.message, type + " content item is missing image_url");
            return;
        }
        append_media(message, server_generation_media_kind::image, source);
        return;
    }
    if (type == "image") {
        const std::string data = string_field(part, "data");
        if (data.empty()) {
            append_recovery(message.message, "MCP image content item is missing data");
            return;
        }
        const std::string mime_type = string_field(part, "mimeType", string_field(part, "mime_type", "image/png"));
        append_media(message, server_generation_media_kind::image, "data:" + mime_type + ";base64," + data);
        return;
    }
    if (type == "input_file") {
        append_file(message, part);
        return;
    }
    append_recovery(message.message, "skipped unsupported content item of type '" +
                                         (type.empty() ? std::string("<unset>") : type) + "'");
}

void append_content(lowered_message & message, const common_json & content) {
    if (content.is_string()) {
        append_text(message.message, content.get<std::string>());
        return;
    }
    if (content.is_array()) {
        for (const common_json & part : content) {
            append_content_item(message, part);
        }
        return;
    }
    if (content.is_object()) {
        if (content.contains("type") && content.at("type").is_string()) {
            append_content_item(message, content);
            return;
        }
        if (content.contains("content")) {
            append_content(message, content.at("content"));
            return;
        }
        for (const char * key : { "body", "output" }) {
            if (content.contains(key) && content.at(key).is_string()) {
                append_text(message.message, content.at(key).get<std::string>());
                return;
            }
        }
    }
    append_text(message.message, content.is_null() ? std::string() : content.dump());
}

common_chat_tool_call make_tool_call(const std::string & name,
                                     const std::string & call_id,
                                     const common_json & arguments) {
    common_chat_tool_call result;
    result.name      = name;
    result.id        = call_id;
    result.arguments = arguments.is_string() ? arguments.get<std::string>() : arguments.dump();
    return result;
}

common_json parse_arguments_best_effort(const common_json & value) {
    if (!value.is_string()) {
        return value.is_null() ? common_json::object() : value;
    }
    try {
        return common_json::parse(value.get<std::string>());
    } catch (const std::exception &) {
        return value;
    }
}

void append_assistant_tool_call(lowering_context & context, common_chat_tool_call tool_call) {
    lowered_message message;
    message.message.role = "assistant";
    message.message.tool_calls.push_back(std::move(tool_call));
    const bool merge = !context.result.chat.messages.empty() && context.result.chat.messages.back().role == "assistant";
    append_lowered_message(context, std::move(message), merge);
}

std::string compaction_summary(const common_json & item) {
    for (const char * key : { "encrypted_content", "summary", "content" }) {
        if (item.contains(key) && item.at(key).is_string()) {
            return item.at(key).get<std::string>();
        }
    }
    if (item.contains("summary") && item.at("summary").is_array()) {
        std::string result;
        for (const common_json & part : item.at("summary")) {
            const std::string text = string_field(part, "text");
            if (!text.empty()) {
                if (!result.empty()) {
                    result += '\n';
                }
                result += text;
            }
        }
        return result;
    }
    return {};
}

void lower_message_item(lowering_context & context, const common_json & item) {
    lowered_message message;
    message.message.role = string_field(item, "role");
    if (message.message.role == "developer") {
        message.message.role = "system";
    }
    if (message.message.role.empty()) {
        throw std::invalid_argument("message input item is missing role");
    }
    if (!item.contains("content")) {
        throw std::invalid_argument("message input item is missing content");
    }
    append_content(message, item.at("content"));

    bool merge = false;
    if (message.message.role == "system" && !context.result.chat.messages.empty() &&
        context.result.chat.messages.front().role == "system") {
        // System/developer messages share the leading model instruction slot.
        if (context.result.chat.messages.size() == 1U) {
            merge = true;
        } else {
            common_chat_msg & first       = context.result.chat.messages.front();
            const std::size_t part_offset = first.content_parts.size();
            first.content_parts.insert(first.content_parts.end(),
                                       std::make_move_iterator(message.message.content_parts.begin()),
                                       std::make_move_iterator(message.message.content_parts.end()));
            for (pending_media & media : message.media) {
                media.content_part_index += part_offset;
                consume_inline_media(context, media.source);
                context.result.media.push_back({
                    0U,
                    media.content_part_index,
                    media.kind,
                    std::move(media.source),
                });
            }
            return;
        }
    } else if (message.message.role == "assistant" && !context.result.chat.messages.empty() &&
               context.result.chat.messages.back().role == "assistant") {
        merge = true;
    }
    append_lowered_message(context, std::move(message), merge);
}

void lower_tool_call(lowering_context & context, const common_json & item, const std::string & type) {
    std::string name      = string_field(item, "name");
    common_json arguments = common_json::object();
    if (type == "custom_tool_call") {
        name               = name.empty() ? "custom_tool" : name;
        arguments["input"] = string_field(item, "input");
    } else if (type == "local_shell_call") {
        name      = "local_shell";
        arguments = item.contains("action") ? item.at("action") : common_json::object();
    } else {
        name = name.empty() ? "function" : name;
        if (item.contains("namespace") && item.at("namespace").is_string()) {
            name = namespace_tool_name(item.at("namespace").get<std::string>(), name);
        }
        arguments = parse_arguments_best_effort(item.contains("arguments") ? item.at("arguments") : common_json());
    }
    const std::string call_id = string_field(item, "call_id", string_field(item, "id"));
    append_assistant_tool_call(context, make_tool_call(name, call_id, arguments));
}

void lower_tool_output(lowering_context & context, const common_json & item) {
    const std::string call_id = string_field(item, "call_id");
    if (call_id.empty()) {
        throw std::invalid_argument("tool output input item is missing call_id");
    }
    lowered_message message;
    message.message.role         = "tool";
    message.message.tool_call_id = call_id;
    message.message.tool_name    = string_field(item, "name");
    if (!item.contains("output")) {
        throw std::invalid_argument("tool output input item is missing output");
    }
    append_content(message, item.at("output"));
    append_lowered_message(context, std::move(message));
}

void lower_reasoning(lowering_context & context, const common_json & item) {
    std::string reasoning;
    if (item.contains("content") && item.at("content").is_string()) {
        reasoning = item.at("content").get<std::string>();
    } else if (item.contains("content") && item.at("content").is_array()) {
        for (const common_json & part : item.at("content")) {
            const std::string text = string_field(part, "text");
            if (!text.empty()) {
                reasoning += text;
            }
        }
    }
    if (!context.result.chat.messages.empty() && context.result.chat.messages.back().role == "assistant") {
        context.result.chat.messages.back().reasoning_content = std::move(reasoning);
        return;
    }
    lowered_message message;
    message.message.role              = "assistant";
    message.message.reasoning_content = std::move(reasoning);
    append_lowered_message(context, std::move(message));
}

void lower_item(lowering_context & context, const common_json & item) {
    if (!item.is_object()) {
        throw std::invalid_argument("Responses input items must be objects after normalization");
    }
    const std::string type = string_field(item, "type");
    if (type == "message" || (type.empty() && item.contains("role"))) {
        lower_message_item(context, item);
    } else if (type == "function_call" || type == "custom_tool_call" || type == "local_shell_call") {
        lower_tool_call(context, item, type);
    } else if (type == "function_call_output" || type == "custom_tool_call_output" || type == "mcp_tool_call_output" ||
               type == "web_search_output" || type == "file_search_output" || type == "tool_search_output" ||
               type == "computer_call_output") {
        lower_tool_output(context, item);
    } else if (type == "reasoning") {
        lower_reasoning(context, item);
    } else if (type == "compaction" || type == "compaction_summary") {
        const std::string summary = compaction_summary(item);
        if (!summary.empty()) {
            lowered_message message;
            message.message.role = "user";
            append_text(message.message, "Previous conversation summary:\n\n" + summary);
            append_lowered_message(context, std::move(message));
        }
    } else if (type == "ghost_snapshot") {
        // IDE-local bookkeeping does not belong in the model prompt.
    } else if (type == "item_reference") {
        throw std::invalid_argument("item_reference reached generation before resolution");
    } else {
        lowered_message recovery;
        recovery.message.role = "assistant";
        append_text(recovery.message, "[responses recovery: skipped unsupported input item of type '" +
                                          (type.empty() ? "<no-type>" : type) + "']");
        append_lowered_message(context, std::move(recovery));
    }
}

common_json object_schema(common_json properties, common_json required = common_json::array()) {
    common_json schema = {
        { "type",                 "object"              },
        { "properties",           std::move(properties) },
        { "additionalProperties", true                  },
    };
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

std::string custom_tool_description(const common_json & tool, const std::string & name) {
    std::string description = string_field(tool, "description", "Freeform Codex tool input.");
    if (name == "apply_patch") {
        description +=
            "\n\nPass only the raw apply_patch body in the input argument. "
            "The body must begin with '*** Begin Patch' and end with '*** End Patch'.";
    }
    if (tool.contains("format") && tool.at("format").is_object()) {
        const common_json & format     = tool.at("format");
        const std::string   definition = string_field(format, "definition");
        if (!definition.empty()) {
            const std::string syntax = string_field(format, "syntax");
            description += "\n\nFreeform input format" + (syntax.empty() ? std::string() : " (" + syntax + ")") +
                           ":\n" + definition;
        }
    }
    return description;
}

common_chat_tool lower_function_tool(const common_json & tool, const std::string & name) {
    common_chat_tool result;
    result.name        = name;
    result.description = string_field(tool, "description");
    result.parameters  = tool.contains("parameters") && tool.at("parameters").is_object() ?
                             tool.at("parameters").dump() :
                             object_schema(common_json::object()).dump();
    return result;
}

common_chat_tool lower_custom_tool(const common_json & tool, const std::string & name) {
    common_chat_tool result;
    result.name        = name;
    result.description = custom_tool_description(tool, name);
    result.parameters  = object_schema(
                             common_json{
                                 { "input",
                                  {
                                       { "type", "string" },
                                       { "description", "Freeform tool input." },
                                   } },
    },
                             common_json::array({ "input" }))
                             .dump();
    return result;
}

void remember_tool(server_generation_input & result,
                   const std::string &       model_name,
                   const common_json &       public_tool,
                   const std::string &       type,
                   const std::string &       public_name,
                   const std::string &       namespace_name = {}) {
    common_json metadata = public_tool;
    metadata["name"]     = public_name;
    metadata["type"]     = type;
    if (!namespace_name.empty()) {
        metadata["namespace"] = namespace_name;
    }
    result.tool_metadata[model_name] = std::move(metadata);
}

void lower_tools(lowering_context & context, const common_json & request) {
    if (!request.contains("tools") || request.at("tools").is_null()) {
        return;
    }
    const common_json & tools = request.at("tools");
    if (!tools.is_array()) {
        throw std::invalid_argument("tools must be an array");
    }
    for (const common_json & tool : tools) {
        const std::string type = string_field(tool, "type");
        if (type == "function") {
            const std::string name = string_field(tool, "name");
            context.result.chat.tools.push_back(lower_function_tool(tool, name));
            remember_tool(context.result, name, tool, type, name);
        } else if (type == "custom") {
            const std::string name = string_field(tool, "name");
            context.result.chat.tools.push_back(lower_custom_tool(tool, name));
            remember_tool(context.result, name, tool, type, name);
        } else if (type == "local_shell") {
            common_chat_tool lowered;
            lowered.name        = "local_shell";
            lowered.description = "Run a local shell command.";
            lowered.parameters =
                object_schema(common_json{
                                  { "command",           { { "type", "array" }, { "items", { { "type", "string" } } } } },
                                  { "cmd",               { { "type", "string" } }                                       },
                                  { "working_directory", { { "type", "string" } }                                       },
                                  { "timeout_ms",        { { "type", "integer" } }                                      },
            })
                    .dump();
            context.result.chat.tools.push_back(std::move(lowered));
            remember_tool(context.result, "local_shell", tool, type, "local_shell");
        } else if (type == "namespace") {
            const std::string namespace_name        = string_field(tool, "name");
            const std::string namespace_description = string_field(tool, "description");
            for (const common_json & nested : tool.at("tools")) {
                const std::string nested_type = string_field(nested, "type");
                const std::string nested_name = string_field(nested, "name");
                const std::string model_name  = namespace_tool_name(namespace_name, nested_name);
                common_chat_tool  lowered     = nested_type == "custom" ? lower_custom_tool(nested, model_name) :
                                                                          lower_function_tool(nested, model_name);
                if (!namespace_description.empty()) {
                    lowered.description = namespace_description +
                                          (lowered.description.empty() ? std::string() : "\n\n" + lowered.description);
                }
                context.result.chat.tools.push_back(std::move(lowered));
                remember_tool(context.result, model_name, nested, nested_type, nested_name, namespace_name);
            }
        } else {
            throw std::invalid_argument("unsupported tool reached generation lowering: " + type);
        }
    }
}

void lower_tool_choice(server_generation_input & result, const common_json & request) {
    if (!request.contains("tool_choice") || request.at("tool_choice").is_null()) {
        return;
    }
    const common_json & choice = request.at("tool_choice");
    if (choice.is_string()) {
        const std::string value = choice.get<std::string>();
        if (value == "none") {
            result.chat.tool_choice = COMMON_CHAT_TOOL_CHOICE_NONE;
        } else if (value == "required") {
            result.chat.tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        } else {
            result.chat.tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
        }
        return;
    }
    const std::string selected = string_field(choice, "name");
    if (selected.empty()) {
        throw std::invalid_argument("named tool_choice is missing name");
    }
    result.chat.tools.erase(std::remove_if(result.chat.tools.begin(), result.chat.tools.end(),
                                           [&](const common_chat_tool & tool) { return tool.name != selected; }),
                            result.chat.tools.end());
    result.chat.tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
}

void lower_structured_output(server_generation_input & result, const common_json & request) {
    if (!request.contains("text") || !request.at("text").is_object() || !request.at("text").contains("format") ||
        !request.at("text").at("format").is_object()) {
        return;
    }
    const common_json & format = request.at("text").at("format");
    const std::string   type   = string_field(format, "type", "text");
    if (type == "json_object") {
        result.chat.json_schema = common_json::object().dump();
    } else if (type == "json_schema") {
        result.chat.json_schema = format.at("schema").dump();
    }
}

void lower_inference_parameters(server_generation_input & result, const common_json & request) {
    for (const char * key : {
             "frequency_penalty",
             "presence_penalty",
             "stream",
             "temperature",
             "top_p",
         }) {
        if (request.contains(key) && !request.at(key).is_null()) {
            result.inference_parameters[key] = request.at(key);
        }
    }
    if (request.contains("max_output_tokens") && !request.at("max_output_tokens").is_null()) {
        result.inference_parameters["max_tokens"] = request.at("max_output_tokens");
    }
    if (request.contains("reasoning") && request.at("reasoning").is_object() &&
        request.at("reasoning").contains("effort") && request.at("reasoning").at("effort").is_string()) {
        result.inference_parameters["reasoning_effort"] = request.at("reasoning").at("effort");
    }
    if (request.contains("parallel_tool_calls") && request.at("parallel_tool_calls").is_boolean()) {
        result.parallel_tool_calls      = request.at("parallel_tool_calls").get<bool>();
        result.chat.parallel_tool_calls = *result.parallel_tool_calls;
    }
    result.inference_parameters["n"] = 1;
}

void lower_item_sequence(lowering_context & context, const common_json & sequence) {
    if (sequence.is_string()) {
        lowered_message message;
        message.message.role = "user";
        append_text(message.message, sequence.get<std::string>());
        append_lowered_message(context, std::move(message));
        return;
    }
    if (sequence.is_object()) {
        lower_item(context, sequence);
        return;
    }
    if (!sequence.is_array()) {
        throw std::invalid_argument("input must be a string, object, or array of items");
    }
    for (const common_json & item : sequence) {
        lower_item(context, item);
    }
}

}  // namespace

server_generation_input lower_responses_generation_input(const common_json & request) {
    if (!request.is_object()) {
        throw std::invalid_argument("Responses generation request must be an object");
    }
    lowering_context context;
    if (request.contains("instructions") && !request.at("instructions").is_null()) {
        if (request.at("instructions").is_string()) {
            lowered_message instructions;
            instructions.message.role = "system";
            append_text(instructions.message, request.at("instructions").get<std::string>());
            append_lowered_message(context, std::move(instructions));
        } else {
            lower_item_sequence(context, request.at("instructions"));
        }
    }
    if (!request.contains("input")) {
        throw std::invalid_argument("input is required after continuation expansion");
    }
    lower_item_sequence(context, request.at("input"));
    lower_tools(context, request);
    lower_tool_choice(context.result, request);
    lower_structured_output(context.result, request);
    lower_inference_parameters(context.result, request);
    std::sort(context.result.media.begin(), context.result.media.end(),
              [](const server_generation_media_source & left, const server_generation_media_source & right) {
                  return std::pair<std::size_t, std::size_t>(left.message_index, left.content_part_index) <
                         std::pair<std::size_t, std::size_t>(right.message_index, right.content_part_index);
              });
    return std::move(context.result);
}

}  // namespace llama_responses
