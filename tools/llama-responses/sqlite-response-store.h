#ifndef LLAMA_RESPONSES_SQLITE_RESPONSE_STORE_H
#define LLAMA_RESPONSES_SQLITE_RESPONSE_STORE_H

#include "response-store.h"

#include <cstddef>
#include <memory>
#include <string>

namespace llama_responses {

// Returns the ordinary llama.cpp cache location and creates its parent
// directory. On Linux the default is ~/.cache/llama.cpp/responses.sqlite3;
// LLAMA_CACHE and XDG_CACHE_HOME retain their normal llama.cpp meanings.
// LLAMA_RESPONSES_DB overrides the complete path for tests and deployments
// that keep response state separate from the model cache.
std::string default_sqlite_response_store_path();

// Durable response storage. The constructor takes an explicit path so tests
// and embedding applications never have to write into the user's cache.
class sqlite_response_store final : public response_store {
  public:
    explicit sqlite_response_store(const std::string & path);
    ~sqlite_response_store() override;

    sqlite_response_store(const sqlite_response_store &)             = delete;
    sqlite_response_store & operator=(const sqlite_response_store &) = delete;

    store_write_result                  create(response_state state) override;
    store_write_result                  replace(response_state state) override;
    std::optional<response_state>       find(const response_id & id) const override;
    std::optional<stored_response_item> find_item(const item_id & id) const override;
    bool                                erase(const response_id & id) override;
    std::size_t                         size() const override;

    // This store deliberately does not resurrect inference after a server
    // restart. Mark snapshots whose process-local worker disappeared as
    // terminal so polling clients never observe an immortal active response.
    std::size_t fail_interrupted_responses();

  private:
    class impl;
    std::unique_ptr<impl> pimpl;
};

}  // namespace llama_responses

#endif  // LLAMA_RESPONSES_SQLITE_RESPONSE_STORE_H
