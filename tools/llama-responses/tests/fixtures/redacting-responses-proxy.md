# Redacting Responses proxy

`redacting-responses-proxy.py` is a development-only reverse proxy for comparing
an isolated Codex session with an OpenAI Responses endpoint. It preserves the
upstream response stream while writing one structural JSON object per completed
HTTP request. It does not record authorization, prompts, input text, tool names,
descriptions, schemas, arguments, outputs, or generated text.

The recorder logs:

- method, route, query-parameter names, and encoded/decoded body sizes;
- model plus non-content mode flags such as `stream`, `store`, reasoning effort,
  and tool-search execution mode;
- run-length-encoded type/status/nullness sequences and their full counts;
- numeric token usage, upstream request ID, response status, and timings.

## Safety boundary

The defaults and startup checks are intentionally inconvenient to misuse:

- the listener binds to `127.0.0.1`;
- `--upstream` is mandatory and must use HTTPS;
- only `/v1/models` and `/v1/responses...` are forwarded;
- the normal `~/.codex` profile is refused;
- the upstream key is read only from an environment variable and is never
  accepted as a command-line option;
- Codex must use a different, ephemeral bearer token, which the proxy replaces
  in memory before forwarding;
- a file sink is opened append-only with mode `0600` and stores no credentials.

HTTP upstreams are accepted only for a literal loopback address and only with
`--allow-http-upstream`, so the unit test can use a fake local service.

## Isolated Codex probe

Create a throwaway profile rather than editing `~/.codex`:

```bash
export CODEX_PROBE_HOME="$HOME/Devel/codex-openai-probe"
mkdir -p "$CODEX_PROBE_HOME"
export CODEX_PROXY_CLIENT_TOKEN="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
```

The profile's `config.toml` can select the loopback provider without persisting
either bearer token:

```toml
model = "gpt-5.6-sol"
model_provider = "openai_probe"

[model_providers.openai_probe]
name = "OpenAI through structural recorder"
base_url = "http://127.0.0.1:8787/v1"
wire_api = "responses"

[model_providers.openai_probe.auth]
command = "/usr/bin/printenv"
args = ["CODEX_PROXY_CLIENT_TOKEN"]
timeout_ms = 5000
refresh_interval_ms = 0
```

Start the proxy in the shell that owns the real `OPENAI_API_KEY`:

```bash
python3 tools/llama-responses/tests/fixtures/redacting-responses-proxy.py \
  --upstream https://api.openai.com/v1 \
  --codex-home "$CODEX_PROBE_HOME" \
  --summary-file /tmp/codex-openai-structural.jsonl
```

Run Codex from a second shell. Remove the upstream key from the child process so
only the proxy can read it:

```bash
env -u OPENAI_API_KEY \
  CODEX_HOME="$CODEX_PROBE_HOME" \
  CODEX_PROXY_CLIENT_TOKEN="$CODEX_PROXY_CLIENT_TOKEN" \
  codex exec 'Reply with exactly OK.'
```

Treat the JSONL file as telemetry even though it is redacted: body sizes, model
names, route timing, and request IDs are still operational metadata. Delete the
throwaway profile and telemetry when the comparison is finished.

## Test

The self-contained test uses a fake loopback upstream, verifies byte-preserving
SSE forwarding and credential replacement, and asserts that planted prompt,
tool, output, query, and credential secrets do not appear in the summary:

```bash
python3 tools/llama-responses/tests/fixtures/test-redacting-responses-proxy.py
```
