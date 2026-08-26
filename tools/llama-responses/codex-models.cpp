#include "codex-models.h"

#include "codex-models-prompt.h"
#include "json.h"
#include "log.h"
#include "server-http.h"
#include "server-route-extensions.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace llama_responses {
namespace {

constexpr const char * CODEX_CLIENT_VERSION_PARAM = "client_version";
constexpr const char * QWEN_LOCAL_ALIAS           = "qwen3.8-27b-local";

bool is_codex_catalog_request(const server_http_req & request) {
    const auto version = request.params.find(CODEX_CLIENT_VERSION_PARAM);
    return version != request.params.end() && !version->second.empty();
}

std::string string_field(const common_json & object, const char * key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) {
        return {};
    }
    return object.at(key).get<std::string>();
}

std::optional<std::int64_t> positive_integer_field(const common_json & object, const char * key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_number_integer()) {
        return std::nullopt;
    }
    const std::int64_t value = object.at(key).get<std::int64_t>();
    return value > 0 ? std::optional<std::int64_t>(value) : std::nullopt;
}

common_json legacy_model_entries(const common_json & legacy_catalog) {
    if (legacy_catalog.is_object() && legacy_catalog.contains("data") && legacy_catalog.at("data").is_array()) {
        return legacy_catalog.at("data");
    }

    common_json result = common_json::array();
    if (!legacy_catalog.is_object() || !legacy_catalog.contains("models") || !legacy_catalog.at("models").is_array()) {
        return result;
    }
    for (const common_json & model : legacy_catalog.at("models")) {
        if (!model.is_object()) {
            continue;
        }
        std::string identifier = string_field(model, "model");
        if (identifier.empty()) {
            identifier = string_field(model, "name");
        }
        if (!identifier.empty()) {
            result.push_back({
                { "id", std::move(identifier) }
            });
        }
    }
    return result;
}

bool string_array_contains(const common_json & values, const char * expected) {
    if (!values.is_array()) {
        return false;
    }
    return std::any_of(values.begin(), values.end(), [expected](const common_json & value) {
        return value.is_string() && value.get<std::string>() == expected;
    });
}

common_json input_modalities(const common_json & legacy_catalog, const common_json & legacy_model, std::size_t index) {
    if (legacy_model.contains("architecture") && legacy_model.at("architecture").is_object()) {
        const common_json & architecture = legacy_model.at("architecture");
        if (architecture.contains("input_modalities") && architecture.at("input_modalities").is_array()) {
            common_json modalities = common_json::array();
            for (const common_json & modality : architecture.at("input_modalities")) {
                if (!modality.is_string()) {
                    continue;
                }
                const std::string value = modality.get<std::string>();
                if (value == "text" || value == "image" || value == "audio") {
                    modalities.push_back(value);
                }
            }
            if (!modalities.empty()) {
                return modalities;
            }
        }
    }

    common_json modalities = common_json::array({ "text" });
    if (!legacy_catalog.contains("models") || !legacy_catalog.at("models").is_array() ||
        index >= legacy_catalog.at("models").size()) {
        return modalities;
    }
    const common_json & ollama_model = legacy_catalog.at("models").at(index);
    if (ollama_model.is_object() && ollama_model.contains("capabilities") &&
        string_array_contains(ollama_model.at("capabilities"), "multimodal") &&
        string_field(legacy_model, "id") == QWEN_LOCAL_ALIAS) {
        modalities.push_back("image");
    }
    return modalities;
}

std::optional<std::int64_t> runtime_context_window(const common_json & legacy_model) {
    if (!legacy_model.is_object() || !legacy_model.contains("meta") || !legacy_model.at("meta").is_object()) {
        return std::nullopt;
    }
    return positive_integer_field(legacy_model.at("meta"), "n_ctx");
}

bool is_private_codex_catalog(const common_json & catalog) {
    if (!catalog.is_object() || !catalog.contains("models") || !catalog.at("models").is_array()) {
        return false;
    }
    const common_json & models = catalog.at("models");
    return models.empty() || (models.front().is_object() && models.front().contains("slug"));
}

common_json qwen_reasoning_levels() {
    return common_json::array({
        {
         { "effort", "low" },
         { "description", "Brief, focused reasoning" },
         },
        {
         { "effort", "medium" },
         { "description", "Balanced reasoning" },
         },
        {
         { "effort", "xhigh" },
         { "description", "Careful, maximum-depth reasoning" },
         },
    });
}

common_json project_model(const common_json & legacy_catalog,
                          const common_json & legacy_model,
                          std::size_t         index,
                          std::size_t         priority) {
    const std::string identifier = string_field(legacy_model, "id");
    const bool        is_qwen    = identifier == QWEN_LOCAL_ALIAS;

    common_json result = {
        { "slug", identifier },
        { "display_name", is_qwen ? "Qwen 3.8 27B Local" : identifier },
        { "description",
         is_qwen ? "Local Qwen3.8-27B served by llama-server." : "Local model served by llama-server." },
        { "supported_reasoning_levels", is_qwen ? qwen_reasoning_levels() : common_json::array() },
        { "shell_type", "unified_exec" },
        { "visibility", "list" },
        { "supported_in_api", true },
        { "priority", priority },
        { "availability_nux", nullptr },
        { "upgrade", nullptr },
        { "base_instructions", CODEX_BASE_INSTRUCTIONS },
        { "include_skills_usage_instructions", false },
        { "include_plugin_usage_instructions", false },
        { "include_apps_usage_instructions", false },
        { "supports_reasoning_summary_parameter", false },
        { "default_reasoning_summary", "none" },
        { "support_verbosity", false },
        { "default_verbosity", nullptr },
        { "apply_patch_tool_type", "freeform" },
        { "truncation_policy", { { "mode", "tokens" }, { "limit", 10000 } } },
        { "supports_image_detail_original", false },
        { "effective_context_window_percent", 95 },
        { "experimental_supported_tools", common_json::array() },
        { "input_modalities", input_modalities(legacy_catalog, legacy_model, index) },
        { "supports_search_tool", is_qwen },
        { "use_responses_lite", is_qwen },
    };

    if (is_qwen) {
        result["default_reasoning_level"] = "low";
    }
    if (const auto context_window = runtime_context_window(legacy_model)) {
        result["context_window"]     = *context_window;
        result["max_context_window"] = *context_window;
    }
    return result;
}

std::optional<common_json> project_catalog(const common_json & legacy_catalog) {
    if (!legacy_catalog.is_object()) {
        return std::nullopt;
    }

    const common_json entries = legacy_model_entries(legacy_catalog);
    common_json       models  = common_json::array();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const common_json & entry      = entries.at(index);
        const std::string   identifier = string_field(entry, "id");
        if (!identifier.empty()) {
            models.push_back(project_model(legacy_catalog, entry, index, index + 1));
        }
    }
    if (models.empty()) {
        return std::nullopt;
    }
    return common_json{
        { "models", std::move(models) }
    };
}

}  // namespace

server_http_handler_decorator make_codex_models_route_decorator() {
    return [](server_http_context::handler_t next_handler) {
        if (!next_handler) {
            throw std::invalid_argument("Codex models decorator requires a next handler");
        }
        return [next_handler = std::move(next_handler)](const server_http_req & request) {
            if (!is_codex_catalog_request(request)) {
                return next_handler(request);
            }

            server_http_res_ptr response = next_handler(request);
            if (!response || response->status != 200 || response->is_stream()) {
                return response;
            }
            try {
                const common_json legacy_catalog = common_json::parse(response->data);
                if (is_private_codex_catalog(legacy_catalog)) {
                    return response;
                }
                const auto catalog = project_catalog(legacy_catalog);
                if (!catalog) {
                    LOG_WRN(
                        "llama-responses: cannot project /v1/models for Codex client_version=%s; using legacy body\n",
                        request.get_param(CODEX_CLIENT_VERSION_PARAM).c_str());
                    return response;
                }
                response->data = catalog->dump();
            } catch (const std::exception & error) {
                LOG_WRN("llama-responses: cannot parse /v1/models for Codex client_version=%s: %s; using legacy body\n",
                        request.get_param(CODEX_CLIENT_VERSION_PARAM).c_str(), error.what());
            }
            return response;
        };
    };
}

}  // namespace llama_responses
