#ifndef LLAMA_SERVER_GENERATION_INTERNAL_H
#define LLAMA_SERVER_GENERATION_INTERNAL_H

#include "server-common.h"
#include "server-generation.h"

#include <memory>
#include <string>
#include <vector>

// Internal adapter into llama-server's shared prompt parser. This is not an
// HTTP Chat Completions seam: the caller supplies typed model input, and this
// function produces the completion engine's established prompt parameters.
json server_generation_params_parse(const server_generation_input & input,
                                    const server_chat_params &      options,
                                    std::vector<raw_buffer> &       files,
                                    bool                            load_media = true);

class server_generation_projection {
  public:
    explicit server_generation_projection(server_generation_sink_ptr sink);

    bool enabled() const noexcept;
    bool cancel_requested() const noexcept;

    std::string accept(const server_task_result & result);
    std::string cancel();
    std::string fail(const std::string & message);
    common_json snapshot() const;

  private:
    struct state;

    std::string accept(const std::vector<server_generation_update> & updates);

    std::shared_ptr<state> state_;
};

#endif  // LLAMA_SERVER_GENERATION_INTERNAL_H
