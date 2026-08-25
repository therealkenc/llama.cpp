#ifndef LLAMA_RESPONSES_HOSTED_TOOLS_H
#define LLAMA_RESPONSES_HOSTED_TOOLS_H

#include "response-types.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace llama_responses {

enum class hosted_tool_kind {
    web_search,
    file_search,
    shell,
    image_generation,
};

const char * hosted_tool_kind_name(hosted_tool_kind kind) noexcept;

struct web_search_request {
    enum class action {
        search,
        open_page,
        find_in_page,
    };

    action                   operation = action::search;
    std::vector<std::string> queries;
    std::string              url;
    std::string              pattern;
};

struct file_search_request {
    std::vector<std::string> queries;
    std::vector<std::string> vector_store_ids;
    std::size_t              max_num_results = 0;
};

struct shell_request {
    std::vector<std::vector<std::string>> commands;
    std::string                           working_directory;
    std::uint64_t                         timeout_ms        = 0;
    std::size_t                           max_output_length = 0;
};

struct image_generation_request {
    std::string prompt;
    std::string size;
    std::string quality;
    std::string background;
};

using hosted_tool_request =
    std::variant<web_search_request, file_search_request, shell_request, image_generation_request>;

hosted_tool_kind hosted_tool_request_kind(const hosted_tool_request & request) noexcept;

struct hosted_tool_capabilities {
    bool                     available             = false;
    bool                     supports_progress     = false;
    bool                     supports_cancellation = false;
    std::size_t              max_concurrency       = 0;
    std::vector<std::string> operations;
    common_json              extensions = common_json::object();
};

struct hosted_tool_content {
    enum class kind {
        text,
        image,
        file,
    };

    kind        type = kind::text;
    std::string text;
    std::string mime_type;
    std::string data;
    std::string filename;
};

struct hosted_tool_result {
    bool                             ok = false;
    std::vector<hosted_tool_content> content;
    common_json                      metadata = common_json::object();
    std::string                      error_code;
    std::string                      error_message;
};

struct hosted_tool_context {
    response_id           response;
    call_id               call;
    std::string           working_directory;
    std::function<bool()> should_stop;
};

class hosted_tool_event_sink {
  public:
    virtual ~hosted_tool_event_sink()                = default;
    virtual void progress(const common_json & event) = 0;
};

// The provider API is independent of its implementation transport. Native
// C++, MCP, direct HTTP, a child process, or a TypeScript bridge all implement
// this same statically-linked strategy boundary.
class hosted_tool_provider {
  public:
    virtual ~hosted_tool_provider()                                             = default;
    virtual hosted_tool_kind         kind() const noexcept                      = 0;
    virtual hosted_tool_capabilities capabilities() const                       = 0;
    virtual hosted_tool_result       invoke(const hosted_tool_request & request,
                                            const hosted_tool_context & context,
                                            hosted_tool_event_sink *    events) = 0;
};

class unavailable_hosted_tool_provider final : public hosted_tool_provider {
  public:
    explicit unavailable_hosted_tool_provider(hosted_tool_kind kind);

    hosted_tool_kind         kind() const noexcept override;
    hosted_tool_capabilities capabilities() const override;
    hosted_tool_result       invoke(const hosted_tool_request & request,
                                    const hosted_tool_context & context,
                                    hosted_tool_event_sink *    events) override;

  private:
    hosted_tool_kind tool_kind;
};

// The default registry is fail-closed: every known kind is backed by an
// unavailable provider until the application explicitly installs a strategy.
class hosted_tool_registry {
  public:
    hosted_tool_registry();

    void                                  install(std::shared_ptr<hosted_tool_provider> provider);
    std::shared_ptr<hosted_tool_provider> provider(hosted_tool_kind kind) const;
    hosted_tool_capabilities              capabilities(hosted_tool_kind kind) const;
    bool                                  available(hosted_tool_kind kind) const;
    hosted_tool_result                    invoke(const hosted_tool_request & request,
                                                 const hosted_tool_context & context,
                                                 hosted_tool_event_sink *    events = nullptr) const;

  private:
    std::map<hosted_tool_kind, std::shared_ptr<hosted_tool_provider>> providers;
};

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_HOSTED_TOOLS_H
