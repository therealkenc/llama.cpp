#include "chat.h"
#include "json.h"
#include "server-generation.h"
#include "server-http.h"
#include "server-integration.h"
#include "server-responses.h"

#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

using namespace llama_responses;

namespace {

int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

struct route_counters {
    int generate = 0;
    int create   = 0;
};

server_http_res_ptr legacy_wire_response() {
    auto response  = std::make_unique<server_http_res>();
    response->data = common_json{
        { "id",           "resp_legacy"         },
        { "object",       "response"            },
        { "created_at",   100                   },
        { "completed_at", 101                   },
        { "status",       "completed"           },
        { "model",        "fixture-model"       },
        { "output",       common_json::array()  },
        { "output_text",  ""                    },
        { "usage",        common_json::object() },
    }.dump();
    return response;
}

server_responses_routes fake_legacy_routes(route_counters & counters) {
    server_responses_routes routes;
    routes.create = [&counters](const server_http_req & /*request*/) {
        ++counters.create;
        return legacy_wire_response();
    };
    routes.generate = [&counters](const server_http_req & request, const server_generation_sink_ptr & sink) {
        ++counters.generate;
        CHECK(sink != nullptr);
        if (!sink) {
            return std::make_unique<server_http_res>();
        }

        sink->accept(server_generation_started{});
        common_chat_msg_diff text;
        text.content_delta = "native fixture";
        sink->accept(server_generation_message_deltas{ { text }, {} });
        common_chat_msg final_message;
        final_message.content = "native fixture";
        sink->accept(server_generation_message_snapshot{ final_message, {} });
        sink->accept(server_generation_completed{ { 7, 2, 3 }, 101 });

        auto response  = std::make_unique<server_http_res>();
        response->data = sink->snapshot().dump();
        if (common_json::parse(request.body).value("stream", false)) {
            response->content_type = "text/event-stream";
            response->next         = [](std::string & output) {
                output.clear();
                return false;
            };
        }
        return response;
    };
    return routes;
}

server_http_req make_request(const std::function<bool()> & should_stop, const common_json & body) {
    return {
        {},
        {},
        "/v1/responses",
        "",
        body.dump(),
        {},
        should_stop,
    };
}

void test_native_route_selection_and_typed_persistence() {
    route_counters counters;
    auto           routes = make_server_responses_routes_factory()(fake_legacy_routes(counters));
    CHECK(routes.generate != nullptr);

    const std::function<bool()> should_stop = [] { return false; };
    server_http_req request = make_request(should_stop, {
                                                            { "model",             "fixture-model" },
                                                            { "input",             "hello"         },
                                                            { "max_output_tokens", 16              },
                                                            { "store",             true            },
                                                        });
    server_http_res_ptr response = routes.create(request);
    CHECK(response != nullptr);
    CHECK(response && response->status == 200);
    CHECK(counters.generate == 1);
    CHECK(counters.create == 0);
    if (!response || response->status != 200) {
        return;
    }

    const common_json snapshot = common_json::parse(response->data);
    CHECK(snapshot.at("output_text") == "native fixture");
    CHECK(snapshot.at("usage").at("input_tokens_details").at("cached_tokens") == 2);

    server_http_req retrieve       = make_request(should_stop, common_json::object());
    retrieve.params["response_id"] = snapshot.at("id").get<std::string>();
    retrieve.path                  = "/v1/responses/{response_id}";
    server_http_res_ptr stored     = routes.retrieve(retrieve);
    CHECK(stored != nullptr);
    CHECK(stored && stored->status == 200);
    if (stored && stored->status == 200) {
        CHECK(common_json::parse(stored->data) == snapshot);
    }
}

void test_telemetry_extension_uses_legacy_oracle() {
    route_counters              counters;
    auto                        routes      = make_server_responses_routes_factory()(fake_legacy_routes(counters));
    const std::function<bool()> should_stop = [] { return false; };
    server_http_req request = make_request(should_stop, {
                                                            { "model",             "fixture-model" },
                                                            { "input",             "hello"         },
                                                            { "max_output_tokens", 16              },
                                                            { "store",             false           },
                                                            { "return_progress",   true            },
                                                        });
    server_http_res_ptr response = routes.create(request);
    CHECK(response != nullptr);
    CHECK(response && response->status == 200);
    CHECK(counters.generate == 0);
    CHECK(counters.create == 1);
    if (response) {
        CHECK(common_json::parse(response->data).at("id") == "resp_legacy");
    }
}

}  // namespace

int main() try {
    test_native_route_selection_and_typed_persistence();
    test_telemetry_extension_uses_legacy_oracle();
    if (failures != 0) {
        std::cerr << failures << " route integration checks failed\n";
        return 1;
    }
    std::cout << "llama-responses route integration checks passed\n";
    return 0;
} catch (const std::exception & error) {
    std::cerr << "route integration checks threw: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "route integration checks threw an unknown exception\n";
    return 1;
}
