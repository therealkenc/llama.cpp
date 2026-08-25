#include "hosted-tools.h"

#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace llama_responses {

const char * hosted_tool_kind_name(hosted_tool_kind kind) noexcept {
    switch (kind) {
        case hosted_tool_kind::web_search:
            return "web_search";
        case hosted_tool_kind::file_search:
            return "file_search";
        case hosted_tool_kind::shell:
            return "shell";
        case hosted_tool_kind::image_generation:
            return "image_generation";
    }
    return "unknown";
}

hosted_tool_kind hosted_tool_request_kind(const hosted_tool_request & request) noexcept {
    if (std::holds_alternative<web_search_request>(request)) {
        return hosted_tool_kind::web_search;
    }
    if (std::holds_alternative<file_search_request>(request)) {
        return hosted_tool_kind::file_search;
    }
    if (std::holds_alternative<shell_request>(request)) {
        return hosted_tool_kind::shell;
    }
    return hosted_tool_kind::image_generation;
}

unavailable_hosted_tool_provider::unavailable_hosted_tool_provider(hosted_tool_kind kind) : tool_kind(kind) {}

hosted_tool_kind unavailable_hosted_tool_provider::kind() const noexcept {
    return tool_kind;
}

hosted_tool_capabilities unavailable_hosted_tool_provider::capabilities() const {
    hosted_tool_capabilities result;
    result.extensions["reason"] = "provider_not_configured";
    return result;
}

hosted_tool_result unavailable_hosted_tool_provider::invoke(const hosted_tool_request & /*request*/,
                                                            const hosted_tool_context & /*context*/,
                                                            hosted_tool_event_sink * /*events*/) {
    hosted_tool_result result;
    result.error_code    = "hosted_tool_provider_unavailable";
    result.error_message = std::string("no provider configured for ") + hosted_tool_kind_name(tool_kind);
    return result;
}

hosted_tool_registry::hosted_tool_registry() {
    for (hosted_tool_kind kind : {
             hosted_tool_kind::web_search,
             hosted_tool_kind::file_search,
             hosted_tool_kind::shell,
             hosted_tool_kind::image_generation,
         }) {
        providers.emplace(kind, std::make_shared<unavailable_hosted_tool_provider>(kind));
    }
}

void hosted_tool_registry::install(std::shared_ptr<hosted_tool_provider> provider) {
    if (!provider) {
        throw std::invalid_argument("hosted tool provider must not be null");
    }
    providers[provider->kind()] = std::move(provider);
}

std::shared_ptr<hosted_tool_provider> hosted_tool_registry::provider(hosted_tool_kind kind) const {
    const auto found = providers.find(kind);
    if (found == providers.end()) {
        // Defensive only: the constructor installs every enum value.
        return std::make_shared<unavailable_hosted_tool_provider>(kind);
    }
    return found->second;
}

hosted_tool_capabilities hosted_tool_registry::capabilities(hosted_tool_kind kind) const {
    return provider(kind)->capabilities();
}

bool hosted_tool_registry::available(hosted_tool_kind kind) const {
    return capabilities(kind).available;
}

hosted_tool_result hosted_tool_registry::invoke(const hosted_tool_request & request,
                                                const hosted_tool_context & context,
                                                hosted_tool_event_sink *    events) const {
    return provider(hosted_tool_request_kind(request))->invoke(request, context, events);
}

}  // namespace llama_responses
