# Codex 0.149.1 Responses Lite create-request fixture

This directory contains a sanitized black-box capture of the standalone Codex
CLI's first `POST /v1/responses` request after its private model catalog
advertised `use_responses_lite: true`. The capture used only a loopback HTTP
listener, a deterministic private model catalog, and a dummy bearer token. It
did not start `llama-server`, load a model, use a GPU, or call an external model
API.

The checked-in JSON is an envelope rather than an unsanitized wire body:

- `request` retains the ten observed top-level fields exactly:
  `client_metadata`, `include`, `input`, `model`, `parallel_tool_calls`,
  `prompt_cache_key`, `reasoning`, `store`, `stream`, and `tool_choice`;
- top-level `instructions` and `tools` are deliberately absent. Responses Lite
  moves both into the ordered `input` sequence;
- `input` contains five items in wire order:

  1. an ID-less, developer-role `additional_tools` item containing one
     `functions` namespace and its six complete declarations;
  2. an ID-less developer message containing the model's base instructions;
  3. an ID-bearing developer message containing generated skill and permission
     context;
  4. an ID-bearing user message containing generated environment context; and
  5. the ID-bearing user capture prompt;
- the six declarations under `additional_tools.tools[0].tools` retain their
  complete schemas, including the custom `apply_patch` grammar;
- dynamic identifiers and generated environment/developer prose have stable
  replacements whose JSON pointers and original fingerprints are recorded in
  `normalization`;
- the base-instructions text at `/request/input/1/content/0/text` references the
  adjacent owned Codex prompt by path, character count, byte count, and SHA-256
  instead of duplicating roughly 20 KiB;
- `capture.transport` records the method, path, body size, header inventory, and
  stable header values without retaining authentication or per-turn IDs.

The captured body was 32,390 bytes both raw and decoded, with no content
encoding. The referenced base instructions were 20,751 characters and 20,903
UTF-8 bytes, with SHA-256
`ac8ae107a0d72fe3476b430afb161ea4e67da2e446d778aefc44828160559807`.
The stable transport values were `Accept: text/event-stream`,
`Content-Type: application/json`, and
`x-openai-internal-codex-responses-lite: true`. Authentication, session, turn,
installation, request, window, user-agent, and beta-feature values remain
redacted or represented only by their names and fingerprints.

The normalized request remains a valid Codex Responses Lite body. Sending it
as-is uses the placeholder strings as ordinary text. A prompt-fidelity replay
should replace `$CODEX_BASE_INSTRUCTIONS` at the recorded input pointer with
the referenced file first.

## Compatibility classification

Every field observed in this capture has a deliberate local disposition:

| Field or surface | Disposition |
| --- | --- |
| `input[0].type: "additional_tools"` | Implemented as ordered, developer-role tool context. Its declarations enter the shared tool-lowering seam and the typed item is retained for storage and replay. |
| Developer messages in `input` | Implemented through the ordinary instruction/message lowering path; no second prompt renderer is used. |
| `model` | Implemented model routing. |
| `tool_choice: "auto"`, `parallel_tool_calls: false` | Implemented typed client-tool negotiation and single-call policy lowering. |
| `reasoning: {"context":"all_turns","effort":"low"}` | `all_turns` matches complete local lineage materialization; the effort value reaches the existing model reasoning control. |
| `stream: true` | Implemented Responses SSE projection. |
| `store: false` | Implemented resource policy; this ordinary foreground response is not retained. Stateful test variants deliberately use `store: true` when retrieval must be inspected. |
| `include: ["reasoning.encrypted_content"]` | Truthful create-time no-op because local generation has no opaque encrypted reasoning payload. |
| `client_metadata` | Validated and preserved as protocol metadata; deliberately excluded from model inference. |
| `prompt_cache_key` | Validated and echoed metadata; llama-server retains ownership of its own prompt-cache policy. |
| `x-openai-internal-codex-responses-lite: true` | Accepted as the observed Codex transport marker. Request semantics come from the body and the advertised catalog capability, not from treating this private header as authentication. |

The 0.149.1 capture therefore exposes no unsupported create field. The version
is fixture provenance, not a routing constraint. On `/v1/models`, any nonempty
`client_version` enters the private Codex catalog decorator; absent, empty, or
unrelated query parameters delegate to llama-server unchanged. The value is
otherwise opaque, and a successfully projected response is a forward-
compatible JSON bag.

## Reproduce the capture

Resolve and record the binary before starting:

```bash
readlink -f "$(command -v codex)"
codex --version
```

In one terminal, start the capture listener. Choose an unused high port if
`18139` is occupied:

```bash
python3 tools/llama-responses/tests/fixtures/capture-codex-create-request.py \
    --port 18139 \
    --capture-date 2026-08-26 \
    --raw-output /tmp/codex-create-request-0.149.1.raw.json \
    --headers-output /tmp/codex-create-request-0.149.1.headers.json \
    --catalog-output /tmp/codex-create-request-0.149.1.catalog.json \
    --normalized-output /tmp/codex-create-request-0.149.1.normalized.json
```

In another terminal, run the CLI against that listener:

```bash
CAPTURE_CODEX_HOME=$(mktemp -d /tmp/codex-create-request.XXXXXX)

env -u OPENAI_API_KEY -u OPENAI_BASE_URL \
    CODEX_HOME="$CAPTURE_CODEX_HOME" \
    CAPTURE_API_KEY=sk-capture-not-secret \
    codex --ask-for-approval never exec \
    --ephemeral \
    --ignore-user-config \
    --ignore-rules \
    --strict-config \
    --sandbox read-only \
    --json \
    --color never \
    -C "$PWD" \
    -m capture-model \
    -c 'model_provider="capture_local"' \
    -c 'model_providers.capture_local.name="Loopback capture"' \
    -c 'model_providers.capture_local.base_url="http://127.0.0.1:18139/v1"' \
    -c 'model_providers.capture_local.auth.command="/usr/bin/printenv"' \
    -c 'model_providers.capture_local.auth.args=["CAPTURE_API_KEY"]' \
    -c 'model_providers.capture_local.requires_openai_auth=false' \
    -c 'model_providers.capture_local.wire_api="responses"' \
    -c 'check_for_update_on_startup=false' \
    -c 'web_search="disabled"' \
    -c 'agents.enabled=false' \
    -c 'mcp_servers={}' \
    -c 'analytics.enabled=false' \
    'Reply with exactly CAPTURE_OK and do not call tools.'
```

The listener's private catalog sets both `supports_search_tool: true` and
`use_responses_lite: true`; the latter is what selects the Lite request shape.
The command is expected to exit nonzero with `capture_complete`. That response
is deliberate and prevents model generation or successful retries.
`--ephemeral` suppresses a rollout, but Codex still initializes local
SQLite/cache state. The temporary `CODEX_HOME` keeps that state and all auth
discovery away from `~/.codex`. Remove that exact temporary directory after
inspection.

## Intended test use

A focused fixture test can:

1. assert the exact ten-field top-level key set and the absence of top-level
   `instructions` and `tools`;
2. assert the five-item input order, the two ID-less Lite prefix items, the
   remaining stable replacement IDs, the message roles, and typed `input_text`
   layout;
3. compare the `functions` namespace and all six nested declarations exactly,
   including schemas and grammar;
4. assert `parallel_tool_calls: false`, `tool_choice: "auto"`,
   `reasoning.context: "all_turns"`, `reasoning.effort: "low"`, `store: false`,
   and `stream: true`;
5. verify the 32,390-byte body measurement, private Lite header, and prompt
   reference's character count, byte count, and SHA-256;
6. replay the Lite prefix through raw streaming HTTP without adding top-level
   tool or instruction surrogates;
7. combine the prefix with a correlated client `tool_search_call` and
   `tool_search_output`, then verify that a `store: true` test variant preserves
   `additional_tools` with an `at_` ID and retains the selected definitions on
   the input-items route; and
8. normalize a future CLI capture using the recorded JSON pointers before
   reporting genuine field, ordering, or tool-negotiation drift.

This particular isolated capture had no installed plugin namespace to search,
so its `additional_tools` item contains the six built-in declarations grouped
under `functions` and no top-level client `tool_search` declaration. The
separate continuation test supplies that declaration and its selected-tool
output explicitly. The six captured declarations contain no session UUIDs or
generated environment text. They are copied byte-for-structure from the raw
request and can therefore be compared deterministically without retaining any
dynamic or private content.
