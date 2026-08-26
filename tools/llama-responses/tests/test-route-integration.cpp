#include "chat.h"
#include "json.h"
#include "server-generation.h"
#include "server-http.h"
#include "server-integration.h"
#include "server-responses.h"

#include <sqlite3.h>
#include <stdlib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

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

class temporary_directory {
  public:
    temporary_directory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("llama-responses-route-integration-" + std::to_string(suffix));
        std::filesystem::create_directories(path);
    }

    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    temporary_directory(const temporary_directory &)             = delete;
    temporary_directory & operator=(const temporary_directory &) = delete;

    std::filesystem::path database_path() const { return path / "responses.sqlite3"; }

  private:
    std::filesystem::path path;
};

class scoped_environment_override {
  public:
    scoped_environment_override(std::string name, const std::string & value) : name(std::move(name)) {
        const char * original = std::getenv(this->name.c_str());
        if (original != nullptr) {
            previous = original;
        }
        if (::setenv(this->name.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("could not set test environment override");
        }
    }

    ~scoped_environment_override() {
        if (previous) {
            (void) ::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            (void) ::unsetenv(name.c_str());
        }
    }

    scoped_environment_override(const scoped_environment_override &)             = delete;
    scoped_environment_override & operator=(const scoped_environment_override &) = delete;

  private:
    std::string                name;
    std::optional<std::string> previous;
};

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

class staged_background_stream_fixture {
  public:
    server_generation_service service() {
        server_generation_service result;
        result.generate = [this](const server_http_req &, const server_generation_input & input,
                                 const server_generation_sink_ptr & sink) {
            if (input.inference_parameters.at("stream") != true) {
                throw std::runtime_error("background stream fixture received non-streaming inference parameters");
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                active_sink = sink;
            }
            started.notify_all();

            auto response          = std::make_unique<server_http_res>();
            response->content_type = "text/event-stream";
            response->next         = [this, sink, emitted = false](std::string & output) mutable {
                if (emitted) {
                    output.clear();
                    return false;
                }
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    while (!released && !sink->cancel_requested()) {
                        release_condition.wait_for(lock, std::chrono::milliseconds(5));
                    }
                }
                if (sink->cancel_requested()) {
                    output = sink->accept(server_generation_cancelled{});
                } else {
                    common_chat_msg_diff text;
                    text.content_delta = "detached background fixture";
                    output             = sink->accept(server_generation_message_deltas{ { text } });
                    common_chat_msg final_message;
                    final_message.content = "detached background fixture";
                    output += sink->accept(server_generation_message_snapshot{ final_message });
                    output += sink->accept(server_generation_completed{
                        { 7, 2, 3 },
                        101,
                    });
                }
                emitted = true;
                return false;
            };
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

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        release_condition.notify_all();
    }

  private:
    std::mutex                 mutex;
    std::condition_variable    started;
    std::condition_variable    release_condition;
    server_generation_sink_ptr active_sink;
    bool                       released = false;
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

std::vector<common_json> parse_sse(const std::string & payload) {
    std::vector<common_json> events;
    std::size_t              position = 0;
    while ((position = payload.find("data: ", position)) != std::string::npos) {
        position += 6U;
        const std::size_t line_end = payload.find('\n', position);
        events.push_back(common_json::parse(payload.substr(position, line_end - position)));
        position = line_end == std::string::npos ? payload.size() : line_end + 1U;
    }
    return events;
}

std::vector<common_json> drain_sse(server_http_res & response) {
    std::vector<common_json> events;
    bool                     more = true;
    while (more) {
        std::string chunk;
        more                          = response.next(chunk);
        std::vector<common_json> next = parse_sse(chunk);
        events.insert(events.end(), std::make_move_iterator(next.begin()), std::make_move_iterator(next.end()));
    }
    return events;
}

std::function<bool()> deadline_stop() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    return [deadline] {
        return std::chrono::steady_clock::now() >= deadline;
    };
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

void test_active_background_response_cannot_be_continued() {
    staged_background_stream_fixture fixture;
    auto                             routes      = make_server_responses_routes_factory()(fixture.service());
    const std::function<bool()>      should_stop = deadline_stop();
    server_http_req                  request     = make_request(should_stop, {
                                                                                 { "model",             "fixture-model" },
                                                                                 { "input",             "start"         },
                                                                                 { "max_output_tokens", 16              },
                                                                                 { "background",        true            },
                                                                                 { "stream",            true            },
                                                                                 { "store",             true            },
    });
    server_http_res_ptr              created     = routes.create(request);
    CHECK(created && created->status == 200 && created->is_stream());
    if (!created || !created->is_stream()) {
        routes.shutdown();
        return;
    }

    std::string head_chunk;
    CHECK(created->next(head_chunk));
    const std::vector<common_json> head = parse_sse(head_chunk);
    CHECK(head.size() == 2U);
    if (head.empty()) {
        fixture.release();
        routes.shutdown();
        return;
    }
    const std::string id = head.front().at("response").at("id").get<std::string>();
    CHECK(fixture.wait_for_sink() != nullptr);

    server_http_req           continuation = make_request(should_stop, {
                                                                           { "model",                "fixture-model" },
                                                                           { "input",                "continue"      },
                                                                           { "previous_response_id", id              },
                                                                           { "max_output_tokens",    16              },
                                                                           { "store",                false           },
    });
    const server_http_res_ptr rejected     = routes.create(continuation);
    CHECK(rejected && rejected->status == 400);
    if (rejected && rejected->status == 400) {
        const common_json error = response_body(rejected).at("error");
        CHECK(error.at("code") == "response_not_complete");
        CHECK(error.at("param") == "previous_response_id");
    }

    fixture.release();
    const std::vector<common_json> tail = drain_sse(*created);
    CHECK(!tail.empty());
    CHECK(!tail.empty() && tail.back().at("type") == "response.completed");
    created->on_complete();
    routes.shutdown();
}

void test_background_stream_detaches_and_resumes(bool store_response) {
    staged_background_stream_fixture fixture;
    auto                             routes = make_server_responses_routes_factory()(fixture.service());

    std::atomic_bool            disconnected{ false };
    const std::function<bool()> create_should_stop = [&disconnected] {
        return disconnected.load();
    };
    server_http_req     request = make_request(create_should_stop, {
                                                                       { "model",             "fixture-model" },
                                                                       { "input",             "hello"         },
                                                                       { "max_output_tokens", 16              },
                                                                       { "background",        true            },
                                                                       { "stream",            true            },
                                                                       { "store",             store_response  },
    });
    server_http_res_ptr created = routes.create(request);
    CHECK(created && created->status == 200);
    CHECK(created && created->is_stream());
    if (!created || !created->is_stream()) {
        routes.shutdown();
        return;
    }

    std::string head_chunk;
    CHECK(created->next(head_chunk));
    const std::vector<common_json> head = parse_sse(head_chunk);
    CHECK(head.size() == 2U);
    if (head.empty()) {
        routes.shutdown();
        return;
    }
    CHECK(head.front().at("type") == "response.created");
    CHECK(head.back().at("type") == "response.in_progress");
    const std::string   id     = head.front().at("response").at("id").get<std::string>();
    const std::uint64_t cursor = head.back().at("sequence_number").get<std::uint64_t>();

    const server_generation_sink_ptr sink = fixture.wait_for_sink();
    CHECK(sink != nullptr);
    disconnected.store(true);
    created->on_complete();
    created.reset();
    CHECK(sink && !sink->cancel_requested());

    fixture.release();

    const std::function<bool()> should_stop = deadline_stop();
    server_http_req             retrieve    = make_request(should_stop, common_json::object());
    retrieve.params["response_id"]          = id;
    retrieve.params["stream"]               = "true";
    retrieve.params["starting_after"]       = std::to_string(cursor);
    retrieve.path                           = "/v1/responses/{response_id}";
    server_http_res_ptr resumed             = routes.retrieve(retrieve);
    CHECK(resumed && resumed->status == 200);
    CHECK(resumed && resumed->is_stream());
    if (!resumed || !resumed->is_stream()) {
        routes.shutdown();
        return;
    }

    const std::vector<common_json> tail = drain_sse(*resumed);
    CHECK(!tail.empty());
    if (!tail.empty()) {
        CHECK(tail.front().at("sequence_number").get<std::uint64_t>() == cursor + 1U);
        CHECK(tail.back().at("type") == "response.completed");
        CHECK(tail.back().at("response").at("id") == id);
    }
    std::uint64_t expected = 0;
    for (const common_json & event : head) {
        CHECK(event.at("sequence_number").get<std::uint64_t>() == expected++);
    }
    for (const common_json & event : tail) {
        CHECK(event.at("sequence_number").get<std::uint64_t>() == expected++);
    }

    retrieve.params.erase("stream");
    retrieve.params.erase("starting_after");
    server_http_res_ptr snapshot = routes.retrieve(retrieve);
    CHECK(snapshot && snapshot->status == 200);
    CHECK(response_body(snapshot).at("status") == "completed");
    CHECK(response_body(snapshot).at("store") == store_response);

    retrieve.params["stream"]         = "true";
    retrieve.params["starting_after"] = std::to_string(expected);
    server_http_res_ptr future_cursor = routes.retrieve(retrieve);
    CHECK(future_cursor && future_cursor->status == 200);
    CHECK(future_cursor && future_cursor->is_stream());
    if (future_cursor && future_cursor->is_stream()) {
        CHECK(drain_sse(*future_cursor).empty());
    }

    retrieve.params.erase("stream");
    retrieve.params.erase("starting_after");
    CHECK(routes.delete_response(retrieve)->status == 200);
    CHECK(routes.retrieve(retrieve)->status == 404);
    routes.shutdown();
}

void test_background_stream_future_cursor_compatibility() {
    staged_background_stream_fixture fixture;
    auto                             routes = make_server_responses_routes_factory()(fixture.service());

    const std::function<bool()> create_should_stop = [] {
        return false;
    };
    server_http_req     request = make_request(create_should_stop, {
                                                                       { "model",             "fixture-model" },
                                                                       { "input",             "hello"         },
                                                                       { "max_output_tokens", 16              },
                                                                       { "background",        true            },
                                                                       { "stream",            true            },
                                                                       { "store",             true            },
    });
    server_http_res_ptr created = routes.create(request);
    CHECK(created && created->status == 200);
    CHECK(created && created->is_stream());
    if (!created || !created->is_stream()) {
        routes.shutdown();
        return;
    }

    std::string head_chunk;
    CHECK(created->next(head_chunk));
    const std::vector<common_json> head = parse_sse(head_chunk);
    CHECK(head.size() == 2U);
    if (head.empty()) {
        routes.shutdown();
        return;
    }
    const std::string id = head.front().at("response").at("id").get<std::string>();
    CHECK(fixture.wait_for_sink() != nullptr);

    const auto                  future_deadline    = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const std::function<bool()> future_should_stop = [future_deadline] {
        return std::chrono::steady_clock::now() >= future_deadline;
    };
    server_http_req retrieve          = make_request(future_should_stop, common_json::object());
    retrieve.path                     = "/v1/responses/{response_id}";
    retrieve.params["response_id"]    = id;
    retrieve.params["stream"]         = "true";
    retrieve.params["starting_after"] = "536";
    server_http_res_ptr active_future = routes.retrieve(retrieve);
    CHECK(active_future && active_future->status == 200);
    CHECK(active_future && active_future->is_stream());

    std::vector<common_json> active_future_events;
    std::atomic_bool         active_future_finished{ false };
    std::thread              active_future_subscriber;
    if (active_future && active_future->is_stream()) {
        active_future_subscriber = std::thread([&] {
            active_future_events = drain_sse(*active_future);
            active_future_finished.store(true);
        });
        // The cursor is beyond the active tail, so retrieval must remain
        // pending instead of finishing merely because no matching row exists
        // yet. The request deadline bounds this check if it regresses.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        CHECK(!active_future_finished.load());
    }

    fixture.release();
    if (active_future_subscriber.joinable()) {
        active_future_subscriber.join();
        CHECK(active_future_finished.load());
        CHECK(active_future_events.empty());
        CHECK(std::chrono::steady_clock::now() < future_deadline);
    }

    const std::vector<common_json> tail = drain_sse(*created);
    CHECK(!tail.empty());
    if (tail.empty()) {
        routes.shutdown();
        return;
    }
    const std::uint64_t last_sequence = tail.back().at("sequence_number").get<std::uint64_t>();
    CHECK(tail.back().at("type") == "response.completed");

    const auto check_empty_terminal_stream = [&](const std::string & starting_after) {
        const std::function<bool()> should_stop = [] {
            return false;
        };
        server_http_req terminal_retrieve          = make_request(should_stop, common_json::object());
        terminal_retrieve.path                     = "/v1/responses/{response_id}";
        terminal_retrieve.params["response_id"]    = id;
        terminal_retrieve.params["stream"]         = "true";
        terminal_retrieve.params["starting_after"] = starting_after;
        server_http_res_ptr response               = routes.retrieve(terminal_retrieve);
        CHECK(response && response->status == 200);
        CHECK(response && response->is_stream());
        if (response && response->is_stream()) {
            CHECK(drain_sse(*response).empty());
        }
    };

    check_empty_terminal_stream(std::to_string(last_sequence));
    check_empty_terminal_stream(std::to_string(last_sequence + 536U));
    check_empty_terminal_stream(std::to_string(std::numeric_limits<std::uint64_t>::max()));

    const auto check_cursor_error = [&](const std::string & starting_after, bool stream) {
        const std::function<bool()> should_stop = [] {
            return false;
        };
        server_http_req invalid          = make_request(should_stop, common_json::object());
        invalid.path                     = "/v1/responses/{response_id}";
        invalid.params["response_id"]    = id;
        invalid.params["starting_after"] = starting_after;
        if (stream) {
            invalid.params["stream"] = "true";
        }
        const server_http_res_ptr response = routes.retrieve(invalid);
        CHECK(response && response->status == 400);
        CHECK(response_body(response).at("error").at("param") == "starting_after");
    };
    check_cursor_error("-1", true);
    check_cursor_error("18446744073709551616", true);
    check_cursor_error("0", false);

    created->on_complete();
    routes.shutdown();
}

void test_background_stream_completes_on_create_subscription() {
    staged_background_stream_fixture fixture;
    auto                             routes      = make_server_responses_routes_factory()(fixture.service());
    const std::function<bool()>      should_stop = deadline_stop();
    server_http_req                  request     = make_request(should_stop, {
                                                                                 { "model",             "fixture-model" },
                                                                                 { "input",             "hello"         },
                                                                                 { "max_output_tokens", 16              },
                                                                                 { "background",        true            },
                                                                                 { "stream",            true            },
                                                                                 { "store",             true            },
    });
    server_http_res_ptr              created     = routes.create(request);
    CHECK(created && created->status == 200 && created->is_stream());
    if (!created || !created->is_stream()) {
        routes.shutdown();
        return;
    }

    std::string head_chunk;
    CHECK(created->next(head_chunk));
    const std::vector<common_json> head = parse_sse(head_chunk);
    CHECK(head.size() == 2U);
    CHECK(fixture.wait_for_sink() != nullptr);
    fixture.release();

    const std::vector<common_json> tail = drain_sse(*created);
    CHECK(!tail.empty());
    if (!head.empty() && !tail.empty()) {
        const std::string id = head.front().at("response").at("id").get<std::string>();
        CHECK(tail.back().at("type") == "response.completed");
        CHECK(tail.back().at("response").at("id") == id);
    }
    std::uint64_t expected = 0;
    for (const common_json & event : head) {
        CHECK(event.at("sequence_number").get<std::uint64_t>() == expected++);
    }
    for (const common_json & event : tail) {
        CHECK(event.at("sequence_number").get<std::uint64_t>() == expected++);
    }
    created->on_complete();
    routes.shutdown();
}

void test_background_stream_reports_a_lost_journal_resource() {
    temporary_directory         directory;
    const std::string           database_path = directory.database_path().string();
    scoped_environment_override database_environment("LLAMA_RESPONSES_DB", database_path);

    staged_background_stream_fixture fixture;
    auto                             routes      = make_server_responses_routes_factory()(fixture.service());
    const std::function<bool()>      should_stop = deadline_stop();
    server_http_req                  request     = make_request(should_stop, {
                                                                                 { "model",             "fixture-model" },
                                                                                 { "input",             "hello"         },
                                                                                 { "max_output_tokens", 16              },
                                                                                 { "background",        true            },
                                                                                 { "stream",            true            },
                                                                                 { "store",             true            },
    });
    server_http_res_ptr              created     = routes.create(request);
    CHECK(created && created->is_stream());
    if (!created || !created->is_stream()) {
        routes.shutdown();
        return;
    }

    std::string head_chunk;
    CHECK(created->next(head_chunk));
    const auto head = parse_sse(head_chunk);
    CHECK(head.size() == 2U);
    if (head.empty()) {
        routes.shutdown();
        return;
    }
    const std::string                id          = head.front().at("response").at("id").get<std::string>();
    const server_generation_sink_ptr active_sink = fixture.wait_for_sink();
    CHECK(active_sink != nullptr);

    sqlite3 * database = nullptr;
    CHECK(sqlite3_open(database_path.c_str(), &database) == SQLITE_OK);
    if (database != nullptr) {
        sqlite3_stmt * statement = nullptr;
        CHECK(sqlite3_prepare_v2(database, "DELETE FROM responses WHERE id=?", -1, &statement, nullptr) == SQLITE_OK);
        if (statement != nullptr) {
            CHECK(sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
            CHECK(sqlite3_step(statement) == SQLITE_DONE);
            CHECK(sqlite3_finalize(statement) == SQLITE_OK);
        }
        CHECK(sqlite3_close(database) == SQLITE_OK);
    }

    fixture.release();
    const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (active_sink && !active_sink->cancel_requested() && std::chrono::steady_clock::now() < failure_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(active_sink && active_sink->cancel_requested());

    const auto tail = drain_sse(*created);
    CHECK(tail.size() == 1U);
    if (!tail.empty()) {
        CHECK(tail.front().at("type") == "error");
        CHECK(tail.front().at("code") == "response_store_error");
        CHECK(tail.front().at("sequence_number") == 2);
    }
    created->on_complete();
    routes.shutdown();
}

void test_nonstream_background_rejects_streamed_retrieval() {
    route_counters              counters;
    auto                        routes      = make_server_responses_routes_factory()(fake_generation_service(counters));
    const std::function<bool()> should_stop = [] {
        return false;
    };
    server_http_req     request = make_request(should_stop, {
                                                                { "model",             "fixture-model" },
                                                                { "input",             "hello"         },
                                                                { "max_output_tokens", 16              },
                                                                { "background",        true            },
                                                                { "stream",            false           },
    });
    server_http_res_ptr created = routes.create(request);
    CHECK(created && created->status == 200);
    if (!created || created->status != 200) {
        routes.shutdown();
        return;
    }
    const std::string id           = response_body(created).at("id").get<std::string>();
    server_http_req   retrieve     = make_request(should_stop, common_json::object());
    retrieve.params["response_id"] = id;
    retrieve.params["stream"]      = "true";
    server_http_res_ptr rejected   = routes.retrieve(retrieve);
    CHECK(rejected && rejected->status == 400);
    CHECK(response_body(rejected).at("error").at("param") == "stream");

    retrieve.params["stream"] = "";
    rejected                  = routes.retrieve(retrieve);
    CHECK(rejected && rejected->status == 400);
    CHECK(response_body(rejected).at("error").at("param") == "stream");

    retrieve.params.erase("stream");
    retrieve.params["include_obfuscation"] = "";
    rejected                               = routes.retrieve(retrieve);
    CHECK(rejected && rejected->status == 400);
    CHECK(response_body(rejected).at("error").at("param") == "include_obfuscation");
    routes.shutdown();
}

void test_background_stream_cancel_delete_lifecycle() {
    staged_background_stream_fixture fixture;
    auto                             routes      = make_server_responses_routes_factory()(fixture.service());
    const std::function<bool()>      should_stop = deadline_stop();
    server_http_req                  request     = make_request(should_stop, {
                                                                                 { "model",             "fixture-model" },
                                                                                 { "input",             "cancel me"     },
                                                                                 { "max_output_tokens", 16              },
                                                                                 { "background",        true            },
                                                                                 { "stream",            true            },
                                                                                 { "store",             true            },
    });
    server_http_res_ptr              created     = routes.create(request);
    CHECK(created && created->is_stream());
    if (!created || !created->is_stream()) {
        routes.shutdown();
        return;
    }
    std::string head_chunk;
    CHECK(created->next(head_chunk));
    const auto head = parse_sse(head_chunk);
    CHECK(head.size() == 2U);
    if (head.size() != 2U) {
        routes.shutdown();
        return;
    }
    const std::string   id     = head.front().at("response").at("id").get<std::string>();
    const std::uint64_t cursor = head.back().at("sequence_number").get<std::uint64_t>();
    CHECK(fixture.wait_for_sink() != nullptr);

    server_http_req resource       = make_request(should_stop, common_json::object());
    resource.params["response_id"] = id;
    CHECK(routes.delete_response(resource)->status == 409);

    server_http_res_ptr cancelled = routes.cancel(resource);
    CHECK(cancelled && cancelled->status == 200);
    CHECK(response_body(cancelled).at("status") == "cancelled");
    CHECK(routes.cancel(resource)->status == 200);

    resource.params["stream"]         = "true";
    resource.params["starting_after"] = std::to_string(cursor);
    server_http_res_ptr resumed       = routes.retrieve(resource);
    CHECK(resumed && resumed->is_stream());
    if (resumed && resumed->is_stream()) {
        const auto tail = drain_sse(*resumed);
        CHECK(tail.size() == 1U);
        if (!tail.empty()) {
            CHECK(tail.front().at("type") == "response.cancelled");
            CHECK(tail.front().at("sequence_number") == cursor + 1U);
        }
    }

    resource.params.erase("stream");
    resource.params.erase("starting_after");
    CHECK(routes.delete_response(resource)->status == 200);
    CHECK(routes.retrieve(resource)->status == 404);
    routes.shutdown();
}

void test_shutdown_cancels_and_joins_background_generation() {
    staged_background_stream_fixture fixture;
    auto                             routes      = make_server_responses_routes_factory()(fixture.service());
    const std::function<bool()>      should_stop = deadline_stop();
    server_http_req                  request     = make_request(should_stop, {
                                                                                 { "model",             "fixture-model"    },
                                                                                 { "input",             "stop at shutdown" },
                                                                                 { "max_output_tokens", 16                 },
                                                                                 { "background",        true               },
                                                                                 { "stream",            true               },
                                                                                 { "store",             true               },
    });
    server_http_res_ptr              created     = routes.create(request);
    CHECK(created && created->status == 200 && created->is_stream());
    if (!created || !created->is_stream()) {
        return;
    }
    std::string head_chunk;
    CHECK(created->next(head_chunk));
    const auto head = parse_sse(head_chunk);
    CHECK(!head.empty());
    if (head.empty()) {
        return;
    }
    const std::string   id     = head.front().at("response").at("id").get<std::string>();
    const std::uint64_t cursor = head.back().at("sequence_number").get<std::uint64_t>();
    CHECK(fixture.wait_for_sink() != nullptr);

    routes.shutdown();

    server_http_req retrieve       = make_request(should_stop, common_json::object());
    retrieve.params["response_id"] = id;
    server_http_res_ptr stored     = routes.retrieve(retrieve);
    CHECK(stored && stored->status == 200);
    CHECK(response_body(stored).at("status") == "cancelled");

    retrieve.params["stream"]         = "true";
    retrieve.params["starting_after"] = std::to_string(cursor);
    server_http_res_ptr resumed       = routes.retrieve(retrieve);
    CHECK(resumed && resumed->is_stream());
    if (resumed && resumed->is_stream()) {
        const auto tail = drain_sse(*resumed);
        CHECK(tail.size() == 1U);
        CHECK(!tail.empty() && tail.front().at("type") == "response.cancelled");
    }
}

}  // namespace

int main() try {
    test_native_route_selection_and_typed_persistence();
    test_unsupported_telemetry_never_reaches_generation();
    test_nullable_parallel_tool_calls_is_sdk_decodable();
    test_foreground_cancellation_and_terminal_outcomes();
    test_completion_can_win_foreground_cancellation_race();
    test_background_resource_lifecycle();
    test_active_background_response_cannot_be_continued();
    test_background_stream_detaches_and_resumes(false);
    test_background_stream_detaches_and_resumes(true);
    test_background_stream_future_cursor_compatibility();
    test_background_stream_completes_on_create_subscription();
    test_background_stream_reports_a_lost_journal_resource();
    test_nonstream_background_rejects_streamed_retrieval();
    test_background_stream_cancel_delete_lifecycle();
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
