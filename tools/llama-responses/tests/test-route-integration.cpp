#include "chat.h"
#include "json.h"
#include "server-generation.h"
#include "server-http.h"
#include "server-integration.h"
#include "server-responses.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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
    int                     generate = 0;
    int                     count    = 0;
    server_generation_input last_input;
};

enum class cancellation_finish {
    cancelled,
    completed,
};

class blocking_generation_fixture {
  public:
    explicit blocking_generation_fixture(cancellation_finish finish) : finish(finish) {}

    server_generation_service service() {
        server_generation_service result;
        result.generate = [this](const server_http_req &            request, const server_generation_input &,
                                 const server_generation_sink_ptr & sink) {
            sink->accept(server_generation_started{});
            {
                std::lock_guard<std::mutex> lock(mutex);
                active_sink = sink;
            }
            started.notify_all();

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!sink->cancel_requested() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!sink->cancel_requested()) {
                throw std::runtime_error("cancellation fixture timed out");
            }

            if (finish == cancellation_finish::cancelled) {
                sink->accept(server_generation_cancelled{});
            } else {
                common_chat_msg final_message;
                final_message.content = "completion won cancellation race";
                sink->accept(server_generation_message_snapshot{ final_message });
                sink->accept(server_generation_completed{
                    { 1, 0, 1 },
                    102,
                });
            }

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
        result.count_input_tokens = [](const server_generation_input &) {
            return std::uint64_t{ 1 };
        };
        return result;
    }

    server_generation_sink_ptr wait_for_sink() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!started.wait_for(lock, std::chrono::seconds(2), [this] { return active_sink != nullptr; })) {
            return nullptr;
        }
        return active_sink;
    }

  private:
    const cancellation_finish  finish;
    std::mutex                 mutex;
    std::condition_variable    started;
    server_generation_sink_ptr active_sink;
};

server_generation_service fake_generation_service(route_counters & counters) {
    server_generation_service service;
    service.generate = [&counters](const server_http_req & request, const server_generation_input & input,
                                   const server_generation_sink_ptr & sink) {
        ++counters.generate;
        counters.last_input = input;
        CHECK(sink != nullptr);
        if (!sink) {
            return std::make_unique<server_http_res>();
        }

        sink->accept(server_generation_started{});
        common_chat_msg_diff text;
        text.content_delta = "native fixture";
        sink->accept(server_generation_message_deltas{ { text } });
        common_chat_msg final_message;
        final_message.content = "native fixture";
        sink->accept(server_generation_message_snapshot{ final_message });
        sink->accept(server_generation_completed{
            { 7, 2, 3 },
            101
        });

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
    service.count_input_tokens = [&counters](const server_generation_input & input) {
        ++counters.count;
        counters.last_input = input;
        return std::uint64_t{ 42 };
    };
    return service;
}

server_http_req make_request(const std::function<bool()> & should_stop, const common_json & body) {
    return {
        {}, {}, "/v1/responses", "", body.dump(), {}, should_stop,
    };
}

common_json response_body(const server_http_res_ptr & response) {
    return response ? common_json::parse(response->data) : common_json::object();
}

void test_native_route_selection_and_typed_persistence() {
    route_counters counters;
    auto           routes = make_server_responses_routes_factory()(fake_generation_service(counters));

    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request  = make_request(should_stop, {
                                                                 { "model",             "fixture-model" },
                                                                 { "input",             "hello"         },
                                                                 { "max_output_tokens", 16              },
                                                                 { "store",             true            },
    });
    server_http_res_ptr response = routes.create(request);
    CHECK(response != nullptr);
    CHECK(response && response->status == 200);
    CHECK(counters.generate == 1);
    CHECK(counters.last_input.chat.messages.size() == 1U);
    CHECK(counters.last_input.chat.messages.at(0).role == "user");
    CHECK(counters.last_input.chat.messages.at(0).content_parts.at(0).text == "hello");
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

    retrieve.query_string = "include%5B%5D=message.output_text.logprobs&include%5B%5D=web_search_call.action.sources";
    stored                = routes.retrieve(retrieve);
    CHECK(stored && stored->status == 200);
    CHECK(response_body(stored).at("output").at(0).at("content").at(0).at("logprobs") == common_json::array());

    retrieve.query_string = "include%5B%5D=not.a.real.projection";
    stored                = routes.retrieve(retrieve);
    CHECK(stored && stored->status == 400);
    CHECK(response_body(stored).at("error").at("code") == "invalid_value");
    CHECK(response_body(stored).at("error").at("param") == "include[0]");

    retrieve.query_string = "include%5B%5D=reasoning.encrypted_content";
    stored                = routes.retrieve(retrieve);
    CHECK(stored && stored->status == 400);
    CHECK(response_body(stored).at("error").at("param") == "include");
    retrieve.query_string.clear();

    server_http_req continuation = make_request(should_stop, {
                                                                 { "model",                "fixture-model"   },
                                                                 { "input",                "next"            },
                                                                 { "previous_response_id", snapshot.at("id") },
                                                                 { "max_output_tokens",    16                },
                                                                 { "store",                false             },
    });
    response                     = routes.create(continuation);
    CHECK(response && response->status == 200);
    CHECK(counters.generate == 2);
    CHECK(counters.last_input.chat.messages.size() == 3U);
    CHECK(counters.last_input.chat.messages.at(0).role == "user");
    CHECK(counters.last_input.chat.messages.at(0).content_parts.at(0).text == "hello");
    CHECK(counters.last_input.chat.messages.at(1).role == "assistant");
    CHECK(counters.last_input.chat.messages.at(1).content_parts.at(0).text == "native fixture");
    CHECK(counters.last_input.chat.messages.at(2).role == "user");
    CHECK(counters.last_input.chat.messages.at(2).content_parts.at(0).text == "next");

    const std::string generated_item_id = snapshot.at("output").at(0).at("id").get<std::string>();
    server_http_req   reference         = make_request(should_stop, {
                                                                        { "model",             "fixture-model"                                                          },
                                                                        { "input",             common_json::array({
                                                                                       {
                                                                                           { "type", "item_reference" },
                                                                                           { "id", generated_item_id },
                                                                                       },
                                                                                   }) },
                                                                        { "max_output_tokens", 16                                                                       },
                                                                        { "store",             false                                                                    },
    });
    response                            = routes.create(reference);
    CHECK(response && response->status == 200);
    CHECK(counters.generate == 3);
    CHECK(counters.last_input.chat.messages.size() == 1U);
    CHECK(counters.last_input.chat.messages.at(0).role == "assistant");
    CHECK(counters.last_input.chat.messages.at(0).content_parts.at(0).text == "native fixture");

    server_http_res_ptr counted = routes.input_tokens(request);
    CHECK(counted && counted->status == 200);
    CHECK(counters.count == 1);
    CHECK(common_json::parse(counted->data).at("input_tokens") == 42);
}

void test_unsupported_telemetry_never_reaches_generation() {
    route_counters              counters;
    auto                        routes      = make_server_responses_routes_factory()(fake_generation_service(counters));
    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request  = make_request(should_stop, {
                                                                 { "model",             "fixture-model" },
                                                                 { "input",             "hello"         },
                                                                 { "max_output_tokens", 16              },
                                                                 { "store",             false           },
                                                                 { "return_progress",   true            },
    });
    server_http_res_ptr response = routes.create(request);
    CHECK(response != nullptr);
    CHECK(response && response->status == 400);
    CHECK(counters.generate == 0);
    if (response) {
        CHECK(common_json::parse(response->data).at("error").at("param") == "return_progress");
    }
}

void test_nullable_parallel_tool_calls_is_sdk_decodable() {
    route_counters counters;
    auto           routes = make_server_responses_routes_factory()(fake_generation_service(counters));

    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request  = make_request(should_stop, {
                                                                 { "model",               "fixture-model" },
                                                                 { "input",               "hello"         },
                                                                 { "max_output_tokens",   16              },
                                                                 { "parallel_tool_calls", nullptr         },
    });
    server_http_res_ptr response = routes.create(request);
    CHECK(response && response->status == 200);
    CHECK(response_body(response).at("parallel_tool_calls") == true);
    CHECK(counters.last_input.parallel_tool_calls == std::optional<bool>(true));

    server_http_req invalid = make_request(should_stop, {
                                                            { "model",               "fixture-model" },
                                                            { "input",               "hello"         },
                                                            { "max_output_tokens",   16              },
                                                            { "parallel_tool_calls", "yes"           },
    });
    response                = routes.create(invalid);
    CHECK(response && response->status == 400);
    CHECK(response_body(response).at("error").at("param") == "parallel_tool_calls");
}

void test_foreground_cancellation_and_terminal_outcomes() {
    blocking_generation_fixture fixture(cancellation_finish::cancelled);
    auto                        routes = make_server_responses_routes_factory()(fixture.service());

    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request = make_request(should_stop, {
                                                                { "model",             "fixture-model"         },
                                                                { "input",             "wait for cancellation" },
                                                                { "max_output_tokens", 16                      },
                                                                { "store",             true                    },
    });
    server_http_res_ptr created;
    std::thread         create_thread([&] { created = routes.create(request); });

    const server_generation_sink_ptr sink = fixture.wait_for_sink();
    CHECK(sink != nullptr);
    if (!sink) {
        create_thread.join();
        return;
    }
    const std::string id = sink->snapshot().at("id").get<std::string>();

    server_http_req cancel_request       = make_request(should_stop, common_json::object());
    cancel_request.params["response_id"] = id;
    cancel_request.path                  = "/v1/responses/{response_id}/cancel";
    server_http_res_ptr cancelled        = routes.cancel(cancel_request);
    create_thread.join();

    CHECK(cancelled && cancelled->status == 200);
    CHECK(response_body(cancelled).at("id") == id);
    CHECK(response_body(cancelled).at("status") == "cancelled");
    CHECK(created && created->status == 200);
    CHECK(response_body(created).at("status") == "cancelled");

    server_http_req retrieve_request       = make_request(should_stop, common_json::object());
    retrieve_request.params["response_id"] = id;
    retrieve_request.path                  = "/v1/responses/{response_id}";
    server_http_res_ptr stored             = routes.retrieve(retrieve_request);
    CHECK(stored && stored->status == 200);
    CHECK(response_body(stored).at("status") == "cancelled");

    server_http_res_ptr repeated = routes.cancel(cancel_request);
    CHECK(repeated && repeated->status == 200);
    CHECK(response_body(repeated).at("status") == "cancelled");

    cancel_request.params["response_id"] = "resp_missing_cancel_fixture";
    server_http_res_ptr missing          = routes.cancel(cancel_request);
    CHECK(missing && missing->status == 404);
    CHECK(response_body(missing).at("error").at("code") == "response_not_found");
}

void test_completion_can_win_foreground_cancellation_race() {
    blocking_generation_fixture fixture(cancellation_finish::completed);
    auto                        routes = make_server_responses_routes_factory()(fixture.service());

    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request = make_request(should_stop, {
                                                                { "model",             "fixture-model"                },
                                                                { "input",             "complete during cancellation" },
                                                                { "max_output_tokens", 16                             },
                                                                { "store",             true                           },
    });
    server_http_res_ptr created;
    std::thread         create_thread([&] { created = routes.create(request); });

    const server_generation_sink_ptr sink = fixture.wait_for_sink();
    CHECK(sink != nullptr);
    if (!sink) {
        create_thread.join();
        return;
    }
    server_http_req cancel_request       = make_request(should_stop, common_json::object());
    cancel_request.params["response_id"] = sink->snapshot().at("id").get<std::string>();
    cancel_request.path                  = "/v1/responses/{response_id}/cancel";
    server_http_res_ptr cancellation     = routes.cancel(cancel_request);
    create_thread.join();

    CHECK(cancellation && cancellation->status == 400);
    CHECK(response_body(cancellation).at("error").at("code") == "response_not_cancellable");
    CHECK(created && created->status == 200);
    CHECK(response_body(created).at("status") == "completed");
}

void test_background_resource_lifecycle() {
    blocking_generation_fixture fixture(cancellation_finish::cancelled);
    auto                        routes = make_server_responses_routes_factory()(fixture.service());

    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request = make_request(should_stop, {
                                                                { "model",             "fixture-model"             },
                                                                { "input",             "run outside HTTP lifetime" },
                                                                { "max_output_tokens", 16                          },
                                                                { "background",        true                        },
                                                                { "store",             false                       },
    });
    server_http_res_ptr created = routes.create(request);
    CHECK(created && created->status == 200);
    if (!created || created->status != 200) {
        return;
    }
    const common_json initial = response_body(created);
    CHECK(initial.at("background") == true);
    CHECK(initial.at("store") == false);
    CHECK(initial.at("status") == "in_progress");
    const std::string id = initial.at("id").get<std::string>();

    const server_generation_sink_ptr sink = fixture.wait_for_sink();
    CHECK(sink != nullptr);
    if (!sink) {
        routes.shutdown();
        return;
    }
    CHECK(sink->snapshot().at("id") == id);

    server_http_req resource_request       = make_request(should_stop, common_json::object());
    resource_request.params["response_id"] = id;
    resource_request.path                  = "/v1/responses/{response_id}";

    server_http_res_ptr retrieved = routes.retrieve(resource_request);
    CHECK(retrieved && retrieved->status == 200);
    CHECK(response_body(retrieved).at("status") == "in_progress");

    server_http_res_ptr active_delete = routes.delete_response(resource_request);
    CHECK(active_delete && active_delete->status == 409);
    CHECK(response_body(active_delete).at("error").at("code") == "response_active");

    resource_request.path         = "/v1/responses/{response_id}/cancel";
    server_http_res_ptr cancelled = routes.cancel(resource_request);
    CHECK(cancelled && cancelled->status == 200);
    CHECK(response_body(cancelled).at("status") == "cancelled");

    server_http_res_ptr repeated = routes.cancel(resource_request);
    CHECK(repeated && repeated->status == 200);
    CHECK(response_body(repeated).at("status") == "cancelled");

    resource_request.path       = "/v1/responses/{response_id}";
    server_http_res_ptr removed = routes.delete_response(resource_request);
    CHECK(removed && removed->status == 200);

    resource_request.path       = "/v1/responses/{response_id}/cancel";
    server_http_res_ptr missing = routes.cancel(resource_request);
    CHECK(missing && missing->status == 404);

    CHECK(routes.shutdown != nullptr);
    routes.shutdown();
}

void test_streaming_background_request_is_honestly_rejected() {
    route_counters counters;
    auto           routes = make_server_responses_routes_factory()(fake_generation_service(counters));

    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request  = make_request(should_stop, {
                                                                 { "model",             "fixture-model" },
                                                                 { "input",             "hello"         },
                                                                 { "max_output_tokens", 16              },
                                                                 { "background",        true            },
                                                                 { "stream",            true            },
    });
    server_http_res_ptr response = routes.create(request);
    CHECK(response && response->status == 400);
    CHECK(response_body(response).at("error").at("param") == "stream");
    CHECK(counters.generate == 0);
}

void test_shutdown_cancels_and_joins_background_generation() {
    blocking_generation_fixture fixture(cancellation_finish::cancelled);
    auto                        routes = make_server_responses_routes_factory()(fixture.service());

    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request = make_request(should_stop, {
                                                                { "model",             "fixture-model"    },
                                                                { "input",             "stop at shutdown" },
                                                                { "max_output_tokens", 16                 },
                                                                { "background",        true               },
                                                                { "store",             true               },
    });
    server_http_res_ptr created = routes.create(request);
    CHECK(created && created->status == 200);
    if (!created || created->status != 200) {
        return;
    }
    const std::string id = response_body(created).at("id").get<std::string>();
    CHECK(fixture.wait_for_sink() != nullptr);

    routes.shutdown();

    server_http_req retrieve       = make_request(should_stop, common_json::object());
    retrieve.params["response_id"] = id;
    server_http_res_ptr stored     = routes.retrieve(retrieve);
    CHECK(stored && stored->status == 200);
    CHECK(response_body(stored).at("status") == "cancelled");
}

}  // namespace

int main() try {
    test_native_route_selection_and_typed_persistence();
    test_unsupported_telemetry_never_reaches_generation();
    test_nullable_parallel_tool_calls_is_sdk_decodable();
    test_foreground_cancellation_and_terminal_outcomes();
    test_completion_can_win_foreground_cancellation_race();
    test_background_resource_lifecycle();
    test_streaming_background_request_is_honestly_rejected();
    test_shutdown_cancels_and_joins_background_generation();
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
