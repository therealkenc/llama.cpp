# Codex 0.149.1 Responses create-request fixture

This directory contains a sanitized black-box capture of the standalone Codex
CLI's first `POST /v1/responses` request. The capture used only a loopback HTTP
listener, a deterministic private model catalog, and a dummy bearer token. It
did not start `llama-server`, load a model, use a GPU, or call an external API.

The checked-in JSON is an envelope rather than an unsanitized wire body:

- `request` retains every observed top-level field and all six tool declarations
  exactly, including complete function schemas and the custom `apply_patch`
  grammar;
- dynamic identifiers and generated environment/developer prose have stable
  replacements whose JSON pointers and original fingerprints are recorded in
  `normalization`;
- `instructions` references the adjacent owned Codex prompt by path, size, and
  SHA-256 instead of duplicating roughly 20 KiB;
- `capture.transport` records the method, path, body size, header inventory, and
  stable header values without retaining authentication or per-turn IDs.

The normalized `request` remains valid Responses JSON. Sending it as-is uses
the placeholder strings as ordinary text. A prompt-fidelity replay should
replace `$CODEX_BASE_INSTRUCTIONS` with the referenced file first.

## Compatibility classification

Every field observed in this capture has a deliberate local disposition:

| Field | Disposition |
| --- | --- |
| `input`, `instructions`, `model` | Implemented model input and routing. |
| `tools`, `tool_choice`, `parallel_tool_calls` | Implemented typed client-tool negotiation and lowering. |
| `reasoning` | Implemented inference control for supported model effort names. |
| `stream` | Implemented Responses SSE projection. |
| `store` | Implemented resource policy; ordinary `false` foreground responses are not retained. |
| `include: ["reasoning.encrypted_content"]` | Truthful create-time no-op because local generation has no opaque encrypted reasoning payload. |
| `client_metadata` | Validated/preserved protocol metadata; deliberately excluded from model inference. |
| `prompt_cache_key` | Validated and echoed metadata; llama-server retains ownership of its own prompt-cache policy. |

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
    --capture-date 2026-08-25 \
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

The command is expected to exit nonzero with `capture_complete`; that response
is deliberate and prevents generation or retries. `--ephemeral` suppresses a
rollout, but Codex still initializes local SQLite/cache state. The temporary
`CODEX_HOME` keeps that state and all auth discovery away from `~/.codex`.
Remove that exact temporary directory after inspection.

## Intended test use

A focused fixture test can:

1. assert the exact top-level request-key set;
2. assert the message roles, typed `input_text` layout, and stable final prompt;
3. compare all six tool objects exactly, including schemas and grammar;
4. verify the prompt reference's character count, byte count, and SHA-256;
5. replay `request` through the Responses policy/lowering boundary with a
   scripted generation service; and
6. normalize a future CLI capture using the recorded JSON pointers before
   reporting genuine field or tool-negotiation drift.

The six tool schemas contain no session UUIDs or generated environment text.
They are copied byte-for-structure from the raw request and can therefore be
compared deterministically without retaining any dynamic or private content.
