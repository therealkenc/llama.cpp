#include "codex-models.h"
#include "json.h"
#include "server-http.h"

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace llama_responses;

// This declaration intentionally has external linkage: test-foundation.cpp
// owns the executable's main() and calls this translation unit's test entry.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_codex_models();

namespace {

int failures = 0;

#define CODEX_MODELS_CHECK(condition)                                                       \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

const std::function<bool()> SHOULD_STOP = [] {
    return false;
};

server_http_req make_request(std::map<std::string, std::string> params = {}) {
    return {
        std::move(params), {}, "/v1/models", {}, {}, {}, SHOULD_STOP,
    };
}

server_http_res_ptr make_response(std::string data, int status = 200) {
    auto response    = std::make_unique<server_http_res>();
    response->status = status;
    response->data   = std::move(data);
    response->headers.emplace("x-legacy-handler", "yes");
    return response;
}

common_json single_model_fixture() {
    return {
        { "models", common_json::array({ {
                        { "name", "qwen3.8-27b-local" },
                        { "model", "qwen3.8-27b-local" },
                        { "capabilities", common_json::array({ "completion", "multimodal" }) },
                    } }) },
        { "object", "list"                  },
        { "data",   common_json::array({ {
                      { "id", "qwen3.8-27b-local" },
                      { "object", "model" },
                      { "meta", { { "n_ctx", 32768 }, { "n_ctx_train", 262144 } } },
                  } })     },
    };
}

void test_delegation() {
    int         calls       = 0;
    std::string legacy_body = R"({"object":"list","data":[]})";
    auto        decorate    = make_codex_models_route_decorator();
    auto        handler     = decorate([&](const server_http_req &) {
        ++calls;
        return make_response(legacy_body);
    });

    auto response = handler(make_request());
    CODEX_MODELS_CHECK(calls == 1);
    CODEX_MODELS_CHECK(response->data == legacy_body);

    response = handler(make_request({
        { "unrelated", "value" }
    }));
    CODEX_MODELS_CHECK(calls == 2);
    CODEX_MODELS_CHECK(response->data == legacy_body);

    response = handler(make_request({
        { "client_version", "" }
    }));
    CODEX_MODELS_CHECK(calls == 3);
    CODEX_MODELS_CHECK(response->data == legacy_body);
}

void test_qwen_projection_accepts_version_bags() {
    int               calls   = 0;
    const std::string legacy  = single_model_fixture().dump();
    auto              handler = make_codex_models_route_decorator()([&](const server_http_req & request) {
        ++calls;
        CODEX_MODELS_CHECK(request.get_param("reload") == "1");
        return make_response(legacy);
    });

    for (const std::string & version :
         { std::string("0.148.0"), std::string("0.149.0-alpha.4.3"), std::string("1.148.0"), std::string("99.0.0"),
           std::string("opaque-preview") }) {
        auto response = handler(make_request({
            { "client_version", version },
            { "reload",         "1"     }
        }));
        CODEX_MODELS_CHECK(response->status == 200);
        CODEX_MODELS_CHECK(response->headers.at("x-legacy-handler") == "yes");

        const common_json body = common_json::parse(response->data);
        CODEX_MODELS_CHECK(body.size() == 1);
        CODEX_MODELS_CHECK(body.at("models").size() == 1);
        const common_json & model = body.at("models").at(0);
        CODEX_MODELS_CHECK(model.at("slug") == "qwen3.8-27b-local");
        CODEX_MODELS_CHECK(model.at("display_name") == "Qwen 3.8 27B Local");
        CODEX_MODELS_CHECK(model.at("description").is_string());
        CODEX_MODELS_CHECK(model.at("default_reasoning_level") == "low");
        CODEX_MODELS_CHECK(model.at("supported_reasoning_levels").size() == 3);
        CODEX_MODELS_CHECK(model.at("supported_reasoning_levels").at(2).at("effort") == "xhigh");
        CODEX_MODELS_CHECK(model.at("visibility") == "list");
        CODEX_MODELS_CHECK(model.at("supported_in_api") == true);
        CODEX_MODELS_CHECK(model.at("availability_nux").is_null());
        CODEX_MODELS_CHECK(model.at("upgrade").is_null());
        CODEX_MODELS_CHECK(model.at("supports_reasoning_summary_parameter") == false);
        CODEX_MODELS_CHECK(model.at("default_reasoning_summary") == "none");
        CODEX_MODELS_CHECK(model.at("support_verbosity") == false);
        CODEX_MODELS_CHECK(model.at("default_verbosity").is_null());
        CODEX_MODELS_CHECK(model.at("shell_type") == "unified_exec");
        CODEX_MODELS_CHECK(model.at("apply_patch_tool_type") == "freeform");
        CODEX_MODELS_CHECK(model.at("truncation_policy").at("mode") == "tokens");
        CODEX_MODELS_CHECK(model.at("truncation_policy").at("limit") == 10000);
        CODEX_MODELS_CHECK(model.at("experimental_supported_tools").empty());
        CODEX_MODELS_CHECK(model.at("supports_search_tool") == true);
        CODEX_MODELS_CHECK(model.at("use_responses_lite") == true);
        CODEX_MODELS_CHECK(model.at("context_window") == 32768);
        CODEX_MODELS_CHECK(model.at("max_context_window") == 32768);
        CODEX_MODELS_CHECK(model.at("input_modalities") == common_json::array({ "text", "image" }));
        const std::string instructions = model.at("base_instructions").get<std::string>();
        CODEX_MODELS_CHECK(instructions.size() > 1000);
        CODEX_MODELS_CHECK(instructions.find("You are a coding agent running in the Codex CLI") == 0);
        CODEX_MODELS_CHECK(instructions.find("Do NOT guess or make up an answer") != std::string::npos);
        CODEX_MODELS_CHECK(instructions.find("AGENTS.md") != std::string::npos);
        CODEX_MODELS_CHECK(instructions.find("apply_patch") != std::string::npos);
    }
    CODEX_MODELS_CHECK(calls == 5);
}

void test_router_projection() {
    const common_json legacy = {
        { "object", "list"            },
        { "data",   common_json::array({
                      {
                          { "id", "qwen3.8-27b-local" },
                          { "architecture", { { "input_modalities", common_json::array({ "text", "image" }) } } },
                          { "meta", { { "n_ctx", 65536 } } },
                      },
                      {
                          { "id", "text-fixture" },
                          { "architecture", { { "input_modalities", common_json::array({ "text" }) } } },
                      },
                  }) },
    };
    auto handler =
        make_codex_models_route_decorator()([&](const server_http_req &) { return make_response(legacy.dump()); });

    auto              response = handler(make_request({
        { "client_version", "0.1.0" }
    }));
    const common_json body     = common_json::parse(response->data);
    CODEX_MODELS_CHECK(body.at("models").size() == 2);
    CODEX_MODELS_CHECK(body.at("models").at(0).at("priority") == 1);
    CODEX_MODELS_CHECK(body.at("models").at(0).at("context_window") == 65536);
    CODEX_MODELS_CHECK(body.at("models").at(1).at("priority") == 2);
    CODEX_MODELS_CHECK(body.at("models").at(1).at("supported_reasoning_levels").empty());
    CODEX_MODELS_CHECK(body.at("models").at(1).at("supports_search_tool") == false);
    CODEX_MODELS_CHECK(body.at("models").at(1).at("use_responses_lite") == false);
    CODEX_MODELS_CHECK(!body.at("models").at(1).contains("default_reasoning_level"));
    CODEX_MODELS_CHECK(!body.at("models").at(1).contains("context_window"));
    CODEX_MODELS_CHECK(body.at("models").at(1).at("input_modalities") == common_json::array({ "text" }));
}

void test_legacy_failures_are_preserved() {
    auto unavailable = make_codex_models_route_decorator()(
        [](const server_http_req &) { return make_response("temporarily unavailable", 503); });
    auto response = unavailable(make_request({
        { "client_version", "0.148.0" }
    }));
    CODEX_MODELS_CHECK(response->status == 503);
    CODEX_MODELS_CHECK(response->data == "temporarily unavailable");

    auto malformed =
        make_codex_models_route_decorator()([](const server_http_req &) { return make_response("{not-json"); });
    response = malformed(make_request({
        { "client_version", "0.148.0" }
    }));
    CODEX_MODELS_CHECK(response->status == 200);
    CODEX_MODELS_CHECK(response->data == "{not-json");

    auto empty_catalog = make_codex_models_route_decorator()(
        [](const server_http_req &) { return make_response(R"({"object":"list","data":[]})"); });
    response = empty_catalog(make_request({
        { "client_version", "0.148.0" }
    }));
    CODEX_MODELS_CHECK(response->data == R"({"object":"list","data":[]})");
}

void test_private_downstream_catalog_is_authoritative() {
    const std::string private_body = R"({
  "models": [{
    "slug": "already-owned",
    "display_name": "Already Owned",
    "description": null,
    "supported_reasoning_levels": [],
    "shell_type": "disabled",
    "visibility": "list",
    "supported_in_api": true,
    "priority": 1,
    "availability_nux": null,
    "upgrade": null,
    "base_instructions": "Owned by the next handler.",
    "support_verbosity": false,
    "default_verbosity": null,
    "apply_patch_tool_type": null,
    "truncation_policy": {"mode": "bytes", "limit": 10000},
    "experimental_supported_tools": []
  }]
})";
    auto              handler =
        make_codex_models_route_decorator()([&](const server_http_req &) { return make_response(private_body); });
    auto response = handler(make_request({
        { "client_version", "future" }
    }));
    CODEX_MODELS_CHECK(response->data == private_body);

    const std::string empty_private_body = R"({ "models": [], "data": [{"future_extension": true}] })";
    handler =
        make_codex_models_route_decorator()([&](const server_http_req &) { return make_response(empty_private_body); });
    response = handler(make_request({
        { "client_version", "future" }
    }));
    CODEX_MODELS_CHECK(response->data == empty_private_body);
}

void test_empty_handler_rejected() {
    bool threw = false;
    try {
        (void) make_codex_models_route_decorator()(server_http_context::handler_t());
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CODEX_MODELS_CHECK(threw);
}

}  // namespace

// This function intentionally has external linkage; test-foundation.cpp owns
// main() so all llama-responses foundation checks share one executable.
// NOLINTNEXTLINE(misc-use-internal-linkage)
int test_codex_models() {
    test_delegation();
    test_qwen_projection_accepts_version_bags();
    test_router_projection();
    test_legacy_failures_are_preserved();
    test_private_downstream_catalog_is_authoritative();
    test_empty_handler_rejected();
    return failures;
}
