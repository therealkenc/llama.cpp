#include "chat.h"
#include "generation.h"
#include "json.h"
#include "response-store.h"
#include "response-types.h"
#include "server-generation-adapter.h"
#include "server-generation.h"
#include "sqlite-response-store.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace llama_responses;

namespace {

enum class store_kind {
    none,
    memory,
    sqlite,
};

struct benchmark_case {
    const char * name;
    std::size_t  chunks;
    std::size_t  static_bytes;
    std::size_t  delta_bytes;
    std::size_t  tombstoned_ancestor_bytes;
};

struct run_result {
    double      milliseconds   = 0.0;
    std::size_t database_bytes = 0;
};

struct graph_benchmark_case {
    std::size_t depth;
    std::size_t direct_text_bytes;
};

struct graph_run_result {
    double      create_milliseconds      = 0.0;
    double      materialize_milliseconds = 0.0;
    std::size_t materialized_items       = 0;
    std::size_t materialized_bytes       = 0;
    std::size_t database_bytes           = 0;
};

constexpr benchmark_case default_cases[] = {
    { "chunk_scaling",      1000, 0,      64, 0      },
    { "chunk_scaling",      2000, 0,      64, 0      },
    { "chunk_scaling",      4000, 0,      64, 0      },
    { "static_input",       250,  204800, 4,  0      },
    { "static_input",       500,  204800, 4,  0      },
    { "static_input",       1000, 204800, 4,  0      },
    { "static_input",       2000, 204800, 4,  0      },
    { "tombstoned_lineage", 1000, 0,      4,  204800 },
    { "tombstoned_lineage", 2000, 0,      4,  204800 },
};

constexpr graph_benchmark_case graph_cases[] = {
    { 64,   256 },
    { 256,  256 },
    { 1024, 256 },
};

const char * store_name(store_kind kind) {
    switch (kind) {
        case store_kind::none:
            return "none";
        case store_kind::memory:
            return "memory";
        case store_kind::sqlite:
            return "sqlite";
    }
    throw std::logic_error("unknown benchmark store kind");
}

generation_response_context make_context(std::size_t static_bytes, std::size_t run_number) {
    const std::string static_input(static_bytes, 'i');

    generation_response_context context;
    context.model      = "benchmark-model";
    context.created_at = 100;
    context.request    = {
        { "background", true          },
        { "model",      context.model },
        { "input",      static_input  },
        { "store",      true          },
        { "stream",     true          },
    };
    context.input_items = common_json::array({
        {
         { "id", "msg_input_benchmark_" + std::to_string(run_number) },
         { "type", "message" },
         { "role", "user" },
         { "content", common_json::array({
                             {
                                 { "type", "input_text" },
                                 { "text", static_input },
                             },
                         }) },
         },
    });
    return context;
}

std::filesystem::path database_path(std::size_t run_number) {
    static const std::string invocation = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("llama-responses-generation-bench-" + invocation + '-' + std::to_string(run_number) + ".sqlite3");
}

std::size_t file_size_if_present(const std::filesystem::path & path) {
    std::error_code error;
    const auto      size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::size_t>(size);
}

void remove_database(const std::filesystem::path & path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-shm", error);
    std::filesystem::remove(path.string() + "-wal", error);
}

run_result run_case(store_kind kind, const benchmark_case & benchmark, std::size_t run_number) {
    const std::filesystem::path path = database_path(run_number);
    remove_database(path);

    std::unique_ptr<response_store> store;
    if (kind == store_kind::memory) {
        store = std::make_unique<in_memory_response_store>();
    } else if (kind == store_kind::sqlite) {
        store = std::make_unique<sqlite_response_store>(path.string());
    }

    generation_response_context                    context = make_context(benchmark.static_bytes, run_number);
    std::unique_ptr<native_server_generation_sink> parent;
    if (benchmark.tombstoned_ancestor_bytes != 0) {
        generation_response_context parent_context =
            make_context(benchmark.tombstoned_ancestor_bytes, run_number + 1000000U);
        parent_context.request["background"] = false;
        parent_context.request["stream"]     = false;
        parent                               = std::make_unique<native_server_generation_sink>(
            std::move(parent_context), "bench_parent_" + std::to_string(run_number), false, store.get());
        (void) parent->accept(server_generation_started{});
        (void) parent->accept(server_generation_completed{ {}, 100 });
        context.previous_response               = parent->id();
        context.request["previous_response_id"] = parent->id().str();
    }

    auto sink = std::make_unique<native_server_generation_sink>(
        std::move(context), "bench_" + std::to_string(run_number), false, store.get(),
        std::unordered_map<std::string, common_json>{}, store != nullptr);

    bool already_started = false;
    if (parent) {
        (void) sink->accept(server_generation_started{});
        if (!store->erase(parent->id())) {
            throw std::runtime_error("benchmark could not tombstone its parent response");
        }
        already_started = true;
    }

    common_chat_msg_diff delta;
    delta.content_delta = std::string(benchmark.delta_bytes, 'd');
    const server_generation_message_deltas message_update{ { delta } };

    const auto started_at = std::chrono::steady_clock::now();
    if (!already_started) {
        (void) sink->accept(server_generation_started{});
    }
    for (std::size_t chunk = 0; chunk < benchmark.chunks; ++chunk) {
        (void) sink->accept(message_update);
    }
    (void) sink->accept(server_generation_completed{
        { benchmark.static_bytes / 4, 0, benchmark.chunks },
        101,
    });
    const auto completed_at = std::chrono::steady_clock::now();

    const common_json snapshot = sink->snapshot();
    if (snapshot.at("status") != "completed" ||
        snapshot.at("output_text").get<std::string>().size() != benchmark.chunks * benchmark.delta_bytes) {
        throw std::runtime_error("benchmark projection produced an invalid terminal snapshot");
    }
    if (store != nullptr) {
        const auto page = store->events_after(sink->id());
        if (!page || page->events.empty() || page->head.status != response_status::completed) {
            throw std::runtime_error("benchmark store did not retain its terminal event journal");
        }
    }

    const double elapsed = std::chrono::duration<double, std::milli>(completed_at - started_at).count();
    store.reset();
    const std::size_t bytes = file_size_if_present(path);
    remove_database(path);
    return { elapsed, bytes };
}

response_state make_graph_node(std::size_t                        index,
                               std::size_t                        direct_text_bytes,
                               std::size_t                        run_number,
                               const std::optional<response_id> & parent) {
    const std::string suffix = std::to_string(run_number) + '_' + std::to_string(index);
    const std::string input_text(direct_text_bytes, 'i');
    const std::string output_text(direct_text_bytes, 'o');

    response_state state;
    state.id                = response_id("resp_graph_" + suffix);
    state.status            = response_status::completed;
    state.created_at        = 100 + index;
    state.completed_at      = state.created_at + 1U;
    state.model             = "benchmark-model";
    state.previous_response = parent;
    state.request           = {
        { "input", input_text  },
        { "model", state.model },
        { "store", true        },
    };
    state.input_items = common_json::array({
        {
         { "id", "msg_graph_input_" + suffix },
         { "type", "message" },
         { "role", "user" },
         { "content", common_json::array({
                             {
                                 { "type", "input_text" },
                                 { "text", input_text },
                             },
                         }) },
         },
    });

    response_output_item output;
    output.id    = item_id("msg_graph_output_" + suffix);
    output.type  = "message";
    output.value = {
        { "type",    "message"              },
        { "role",    "assistant"            },
        { "content", common_json::array({
                         {
                             { "type", "output_text" },
                             { "text", output_text },
                         },
                     }) },
    };
    state.output.push_back(std::move(output));
    return state;
}

graph_run_result run_graph_case(store_kind kind, const graph_benchmark_case & benchmark, std::size_t run_number) {
    const std::filesystem::path path = database_path(run_number);
    remove_database(path);

    std::unique_ptr<response_store> store;
    if (kind == store_kind::memory) {
        store = std::make_unique<in_memory_response_store>();
    } else if (kind == store_kind::sqlite) {
        store = std::make_unique<sqlite_response_store>(path.string());
    } else {
        throw std::invalid_argument("graph benchmark requires a response store");
    }

    std::optional<response_id> parent;
    response_id                leaf;
    const auto                 create_started_at = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < benchmark.depth; ++index) {
        response_state state = make_graph_node(index, benchmark.direct_text_bytes, run_number, parent);
        leaf                 = state.id;
        if (store->create(std::move(state)) != store_write_result::stored) {
            throw std::runtime_error("benchmark could not create a response graph node");
        }
        if (parent && !store->erase(*parent)) {
            throw std::runtime_error("benchmark could not tombstone a response graph ancestor");
        }
        parent = leaf;
    }
    const auto create_completed_at = std::chrono::steady_clock::now();

    constexpr std::size_t materialization_samples = 5;
    common_json           materialized;
    const auto            materialize_started_at = std::chrono::steady_clock::now();
    for (std::size_t sample = 0; sample < materialization_samples; ++sample) {
        auto result = store->materialize_continuation_context(leaf);
        if (!result) {
            throw std::runtime_error("benchmark could not materialize its response graph");
        }
        materialized = std::move(*result);
    }
    const auto materialize_completed_at = std::chrono::steady_clock::now();

    if (store->size() != 1 || !materialized.is_array() || materialized.size() != benchmark.depth * 2U) {
        throw std::runtime_error("benchmark response graph produced an invalid materialized view");
    }

    graph_run_result result;
    result.create_milliseconds =
        std::chrono::duration<double, std::milli>(create_completed_at - create_started_at).count();
    result.materialize_milliseconds =
        std::chrono::duration<double, std::milli>(materialize_completed_at - materialize_started_at).count() /
        static_cast<double>(materialization_samples);
    result.materialized_items = materialized.size();
    result.materialized_bytes = materialized.dump().size();
    store.reset();
    result.database_bytes = file_size_if_present(path);
    remove_database(path);
    return result;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t midpoint = values.size() / 2;
    if (values.size() % 2 != 0) {
        return values[midpoint];
    }
    return (values[midpoint - 1] + values[midpoint]) / 2.0;
}

std::size_t parse_repetitions(int argc, char ** argv) {
    if (argc == 1) {
        return 3;
    }
    if (argc == 3 && std::string(argv[1]) == "--repetitions") {
        const unsigned long parsed = std::stoul(argv[2]);
        if (parsed == 0) {
            throw std::invalid_argument("repetitions must be positive");
        }
        return static_cast<std::size_t>(parsed);
    }
    throw std::invalid_argument("usage: bench-llama-responses-generation [--repetitions N]");
}

}  // namespace

int main(int argc, char ** argv) {
    try {
        const std::size_t repetitions = parse_repetitions(argc, argv);
        const store_kind  stores[]    = { store_kind::none, store_kind::memory, store_kind::sqlite };
        std::size_t       run_number  = 0;

        std::cout << "scenario,store,chunks,static_bytes,delta_bytes,tombstoned_ancestor_bytes,repetitions,median_ms,"
                     "min_ms,max_ms,"
                     "median_us_per_chunk,database_bytes\n";
        for (const benchmark_case & benchmark : default_cases) {
            for (store_kind kind : stores) {
                if (benchmark.tombstoned_ancestor_bytes != 0 && kind == store_kind::none) {
                    continue;
                }
                std::vector<double> durations;
                durations.reserve(repetitions);
                std::size_t bytes = 0;
                for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
                    const run_result result = run_case(kind, benchmark, ++run_number);
                    durations.push_back(result.milliseconds);
                    bytes = std::max(bytes, result.database_bytes);
                }

                const auto [minimum, maximum] = std::minmax_element(durations.begin(), durations.end());
                const double middle           = median(durations);
                std::cout << benchmark.name << ',' << store_name(kind) << ',' << benchmark.chunks << ','
                          << benchmark.static_bytes << ',' << benchmark.delta_bytes << ','
                          << benchmark.tombstoned_ancestor_bytes << ',' << repetitions << ',' << std::fixed
                          << std::setprecision(3) << middle << ',' << *minimum << ',' << *maximum << ','
                          << middle * 1000.0 / static_cast<double>(benchmark.chunks) << ',' << bytes << '\n';
            }
        }

        const store_kind graph_stores[] = { store_kind::memory, store_kind::sqlite };
        std::cout << '\n'
                  << "scenario,store,depth,direct_text_bytes,repetitions,median_create_ms,median_create_us_per_node,"
                     "median_materialize_ms,materialized_items,materialized_bytes,database_bytes\n";
        for (const graph_benchmark_case & benchmark : graph_cases) {
            for (store_kind kind : graph_stores) {
                std::vector<double> create_durations;
                std::vector<double> materialize_durations;
                create_durations.reserve(repetitions);
                materialize_durations.reserve(repetitions);
                std::size_t materialized_items = 0;
                std::size_t materialized_bytes = 0;
                std::size_t database_bytes     = 0;
                for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
                    const graph_run_result result = run_graph_case(kind, benchmark, ++run_number);
                    create_durations.push_back(result.create_milliseconds);
                    materialize_durations.push_back(result.materialize_milliseconds);
                    materialized_items = std::max(materialized_items, result.materialized_items);
                    materialized_bytes = std::max(materialized_bytes, result.materialized_bytes);
                    database_bytes     = std::max(database_bytes, result.database_bytes);
                }

                const double create_middle      = median(create_durations);
                const double materialize_middle = median(materialize_durations);
                std::cout << "tombstoned_graph," << store_name(kind) << ',' << benchmark.depth << ','
                          << benchmark.direct_text_bytes << ',' << repetitions << ',' << std::fixed
                          << std::setprecision(3) << create_middle << ','
                          << create_middle * 1000.0 / static_cast<double>(benchmark.depth) << ',' << materialize_middle
                          << ',' << materialized_items << ',' << materialized_bytes << ',' << database_bytes << '\n';
            }
        }
        return 0;
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
