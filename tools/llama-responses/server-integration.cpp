#include "server-integration.h"

#include "hosted-tools.h"
#include "json.h"
#include "log.h"
#include "protocol-codec.h"
#include "response-service.h"
#include "response-store.h"
#include "response-types.h"
#include "server-http.h"
#include "server-responses.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
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
    if (code == "not_supported") {
        type = "not_supported_error";
    } else if (status >= 500) {
        type = "server_error";
    }
    return json_response(render_error(message, type, param, code), status);
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

class responses_routes_impl final : public std::enable_shared_from_this<responses_routes_impl> {
  public:
    explicit responses_routes_impl(server_responses_routes legacy) : legacy(std::move(legacy)), resources(store) {}

    static server_responses_routes routes(const std::shared_ptr<responses_routes_impl> & self) {
        server_responses_routes result;
        result.owner  = self;
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
        common_json                      original    = common_json::object();
        common_json                      input_items = common_json::array();
    };

    server_responses_routes    legacy;
    in_memory_response_store   store;
    response_resource_service  resources;
    // The registry establishes the native C++ strategy boundary now. Its
    // default providers are deliberately unavailable until Phase 4 adapters
    // are configured.
    hosted_tool_registry       hosted_tools;
    std::atomic<std::uint64_t> next_input_id{ 1 };

    item_id make_input_id(std::size_t /*index*/, const common_json & item) {
        std::string prefix = "item_";
        if (item.is_object() && item.value("type", std::string()) == "message") {
            prefix = "msg_";
        }
        return item_id(prefix + "input_" + std::to_string(next_input_id.fetch_add(1)));
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
            const auto resolved = store.find_item(item_id(id));
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
        std::vector<response_state> chain;
        std::set<std::string>       visited;
        std::optional<response_id>  current = previous;
        std::optional<common_json>  detached_context;

        while (current) {
            if (!visited.insert(current->str()).second) {
                throw std::invalid_argument("previous_response_id chain contains a cycle");
            }
            const auto state = store.find(*current);
            if (!state) {
                throw std::out_of_range(current->str());
            }
            chain.push_back(*state);
            if (state->detached_context) {
                detached_context = state->detached_context;
                break;
            }
            current = state->previous_response;
        }

        if (detached_context) {
            for (const common_json & item : *detached_context) {
                expanded.push_back(item);
            }
        }
        std::reverse(chain.begin(), chain.end());
        for (const response_state & state : chain) {
            for (const common_json & item : state.input_items) {
                expanded.push_back(item);
            }
            for (const response_output_item & item : state.output) {
                expanded.push_back(render_output_item(item));
            }
        }
    }

    prepared_request prepare(const server_http_req & request) {
        common_json original = parse_json_body(request);
        if (original.contains("store") && !original.at("store").is_null() && !original.at("store").is_boolean()) {
            throw std::invalid_argument("'store' must be a boolean or null");
        }
        if (original.value("background", false)) {
            throw std::invalid_argument("background responses are not available in the in-memory server profile");
        }
        if (original.contains("conversation") && !original.at("conversation").is_null()) {
            throw std::invalid_argument("conversation state is not available in the in-memory server profile");
        }
        if (original.contains("instructions") && !original.at("instructions").is_null() &&
            !original.at("instructions").is_string() && !original.at("instructions").is_array()) {
            throw std::invalid_argument("'instructions' must be a string, an array of items, or null");
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
            normalized, [this](std::size_t index, const common_json & item) { return make_input_id(index, item); });
        std::set<std::string> current_ids;
        for (const common_json & item : current_items) {
            const std::string id = item.value("id", std::string());
            if (id.empty() || !current_ids.insert(id).second) {
                throw std::invalid_argument("Responses input item ids must be non-empty and unique");
            }
        }
        common_json expanded = common_json::array();

        if (!previous.empty()) {
            append_response_context(response_id(previous), expanded);
        }
        for (const auto & item : current_items) {
            expanded.push_back(item);
        }

        common_json forwarded = original;
        forwarded["input"]    = expanded;
        if (normalized.contains("instructions")) {
            forwarded["instructions"] = normalized.at("instructions");
        }
        // The legacy generation adapter lowers the expanded items to the model,
        // while this hidden field keeps the public response envelope tied to the
        // caller's unexpanded request.
        forwarded["__llama_responses_request"] = original;

        auto forwarded_request  = std::make_shared<server_http_req>(request);
        forwarded_request->body = forwarded.dump();
        return { std::move(forwarded_request), std::move(original), std::move(current_items) };
    }

    void capture_response(const common_json & wire_response,
                          const common_json & original,
                          const common_json & input_items) {
        if (!request_store_enabled(original)) {
            return;
        }
        try {
            response_state           state  = capture_response_state(wire_response, original, input_items);
            const store_write_result result = store.create(std::move(state));
            if (result != store_write_result::stored) {
                LOG_WRN("llama-responses: response storage failed: %s\n", store_write_result_name(result));
            }
        } catch (const std::exception & error) {
            // Storage is an auxiliary resource operation. A completed model
            // response must not be replaced by a transport error if an unknown
            // future output item cannot yet be indexed.
            LOG_WRN("llama-responses: response storage failed: %s\n", error.what());
        }
    }

    void capture_sse(const std::string & payload, const common_json & original, const common_json & input_items) {
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
                            capture_response(event.at("response"), original, input_items);
                        }
                    } catch (const std::exception & error) {
                        // A complete `data:` frame from our generator should
                        // contain JSON. Storage remains auxiliary, so preserve
                        // the client stream but make malformed frames visible.
                        LOG_WRN("llama-responses: ignoring malformed SSE data while storing response: %s\n",
                                error.what());
                    }
                }
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    void capture_sse_chunk(std::string &       pending,
                           const std::string & output,
                           const common_json & original,
                           const common_json & input_items) {
        pending += output;
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
            capture_sse(pending.substr(0, frame_size), original, input_items);
            pending.erase(0, frame_size);
        }
    }

    server_http_res_ptr create(const server_http_req & request) {
        if (!legacy.create) {
            return api_error(501, "Responses create is not available", "not_supported");
        }

        prepared_request prepared;
        try {
            prepared = prepare(request);
        } catch (const std::out_of_range & error) {
            return json_response(render_response_not_found(response_id(error.what())), 404);
        } catch (const std::exception & error) {
            return api_error(400, error.what(), "invalid_request");
        }

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
            if (response->status >= 200 && response->status < 300 && !response->data.empty()) {
                try {
                    capture_response(common_json::parse(response->data), prepared.original, prepared.input_items);
                } catch (const std::exception & error) {
                    // Generation already succeeded. Do not replace its body
                    // with a storage error, but record why retrieval will not
                    // be available for this response.
                    LOG_WRN("llama-responses: ignoring malformed response body while storing response: %s\n",
                            error.what());
                }
            }
            return response;
        }

        response->chunk_observer = [self = shared_from_this(), original = std::move(prepared.original),
                                    input_items = std::move(prepared.input_items),
                                    pending     = std::string()](const std::string & output) mutable {
            self->capture_sse_chunk(pending, output, original, input_items);
        };
        return response;
    }

    server_http_res_ptr input_tokens(const server_http_req & request) {
        if (!legacy.input_tokens) {
            return api_error(501, "Responses input_tokens is not available", "not_supported");
        }
        try {
            prepared_request prepared = prepare(request);
            return legacy.input_tokens(*prepared.request);
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
                501, "Streaming or projected response retrieval is not available in the in-memory server profile",
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
        return api_error(501, "Response compaction is not available in the in-memory server profile", "not_supported");
    }

    server_http_res_ptr input_items(const server_http_req & request) {
        if (!request.get_param("include").empty()) {
            return api_error(501, "Projected input-item retrieval is not available", "not_supported");
        }
        input_item_page_options options;
        try {
            const std::string limit = request.get_param("limit");
            if (!limit.empty()) {
                std::size_t parsed = 0;
                options.limit      = static_cast<std::size_t>(std::stoull(limit, &parsed));
                if (parsed != limit.size()) {
                    throw std::invalid_argument("invalid limit");
                }
            }
            const std::string order = request.get_param("order", "desc");
            if (order != "asc" && order != "desc") {
                throw std::invalid_argument("invalid order");
            }
            options.order           = order == "asc" ? input_item_order::ascending : input_item_order::descending;
            const std::string after = request.get_param("after");
            if (!after.empty()) {
                options.after = item_id(after);
            }
        } catch (const std::exception & error) {
            LOG_DBG("llama-responses: invalid input-items pagination parameter: %s\n", error.what());
            return api_error(400, "Invalid input-items pagination parameter", "invalid_parameter");
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

}  // namespace llama_responses
