# A first-class OpenAI Responses API in `llama-server`

Status: architecture checkpoints 1 and 2 complete; conformance groundwork and
Phases 3 and 3.5 in progress
Spec snapshot: 2026-08-24

## Decision

We will use dependency inversion to put a new, statically linked Responses
subsystem inside the existing `llama-server` executable.

The subsystem will live under `tools/llama-responses/` and build as a library,
not as another executable. The existing `llama-server` remains responsible for
command-line parsing, process lifecycle, authentication, HTTP, model loading,
router mode, slots, metrics, and every non-Responses endpoint. The new subsystem
will own the OpenAI Responses protocol, state machine, resources, streaming
events, and hosted-tool orchestration.

In terms of the three choices:

1. Keep patching `llama-server`: retain this implementation temporarily as a
   behavioral oracle and rollback path, but do not keep extending it as the
   permanent design.
2. Rewrite `llama-server`: reject. It would duplicate mature behavior we need,
   greatly expand our fork, and make routine upstream merges needlessly hard.
3. Insert a Responses subsystem through narrow dependency-inversion seams:
   choose this. It preserves one executable and confines most fork-owned code
   to a coherent module.

This is a source-level sidecar, if that term is useful, but emphatically not an
HTTP sidecar. There is no `llama-responses -> /v1/chat/completions` request. The
product boundary is `/v1/responses` from request through response.

## Goal

For supported capabilities, a client should be unable to distinguish this
server from OpenAI by examining:

- accepted request shapes and validation behavior;
- response objects, item types, IDs, references, status transitions, and usage;
- SSE and WebSocket event types, ordering, indices, and sequence numbers;
- continuation, storage, retrieval, deletion, cancellation, compaction, and
  background behavior;
- function/custom tool round trips and server-hosted tool execution;
- error status, shape, code, parameter attribution, and cancellation behavior.

This does not mean producing identical model tokens. It means implementing the
same observable protocol and lifecycle for every capability we advertise.
Backend or provider limitations must result in explicit, compatible errors or
capability policy. We must never silently drop an input item or pretend to have
executed an unavailable hosted tool.

The current OpenAI reference is the source of truth, including the
[create operation](https://developers.openai.com/api/reference/cli/resources/responses/methods/create)
and the broader
[Responses resource](https://developers.openai.com/api/reference/cli/resources/beta/subresources/responses).
Because that contract changes, conformance is tied to the dated snapshot above
and the snapshot must be deliberately advanced.

## Non-negotiable constraints

- Ship one executable: `llama-server`.
- Do not reimplement `llama-server` CLI parsing or its existing endpoints.
- Do not implement Responses by calling or parsing the Chat Completions HTTP
  endpoint.
- Model prompt rendering may still use shared `common_chat_msg` and chat-template
  facilities. That is an internal inference representation, not a Chat
  Completions compatibility layer.
- Treat Responses items as typed data until the last responsible moment. In
  particular, media tool results must not be flattened to text or forced into
  a user-message wire shape inside the protocol layer.
- Establish C++ hosted-tool interfaces from the beginning. MCP, native C++,
  HTTP, subprocess, and language bridges are possible provider strategies, not
  part of the Responses API itself.
- Keep changes to upstream-owned server files small, unsurprising, and easy to
  replay after an upstream merge.
- Preserve the current fork's useful behavior while the new implementation is
  brought up. Compatibility improvements must be proven by tests, not by
  deleting the working path first.

## Why the current implementation is not the destination

The current route does not call Chat Completions over HTTP. It does, however,
convert a Responses request into Chat Completions JSON, parse that JSON, and
then call `handle_completions_impl(..., TASK_RESPONSE_TYPE_OAI_RESP)`. Responses
serialization and event construction are consequently mixed into completion
task processing.

That has been productive, and it already supports much more than upstream, but
it makes every new Responses feature cut across several concerns:

- request conversion in `server-chat.cpp`;
- route orchestration in `server-context.cpp`;
- generation and Responses event construction in `server-task.cpp`;
- item-reference state in a separate cache;
- special headers and tool-wrapper knowledge in the HTTP route;
- model and router behavior shared with unrelated endpoints.

The pain is architectural rather than cosmetic. Background responses,
retrieval, cancellation, parallel hosted tools, resumable streaming, and real
conversation state would make those files more overloaded. Refactoring the
largest functions in place would still leave Responses protocol knowledge in
the inference engine and would continue to produce broad merge conflicts.

## Target architecture

```text
                         existing llama-server executable

 client
   |
   v
 existing HTTP/auth/router/CLI infrastructure
   |
   v
 Responses route adapter  ---------------------- other existing routes
   |
   v
 llama_responses::service
   |          |              |                 |
   |          |              |                 +--> hosted_tool_registry
   |          |              +--> response_store       |--> MCP provider
   |          +--> event/state machine                 |--> native provider
   +--> request/item model                             |--> bridge provider
   |
   v
 neutral generation_port
   |
   v
 existing slots/task queue/model/chat-template runtime
```

There are two inversion seams.

### Route seam

`server_routes` already exposes assignable handlers, and router mode already
replaces them. We will formalize that pattern with a Responses route bundle or
factory. `server.cpp` constructs the fork-owned implementation after the normal
server context exists and before HTTP routes are registered.

The route bundle eventually owns handlers for:

- `POST /v1/responses`;
- `POST /v1/responses/input_tokens`;
- `GET /v1/responses/{response_id}`;
- `DELETE /v1/responses/{response_id}`;
- `POST /v1/responses/{response_id}/cancel`;
- `POST /v1/responses/compact`;
- `GET /v1/responses/{response_id}/input_items`;
- streaming and WebSocket entry points required by the current contract.

Unversioned aliases may remain for compatibility, but `/v1` behavior is the
conformance target.

The exact C++ spelling should be chosen during the seam spike. The important
properties are:

- construction is explicit and statically linked;
- handler lifetime is owned by `llama_server()`;
- no Responses JSON types leak into the generic HTTP/router layer;
- a legacy route bundle can be injected in tests during migration;
- router mode can proxy the same route bundle without duplicating protocol code.

### Generation seam

Responses needs a narrow, protocol-neutral way to submit inference work and
observe it. It must not call `handle_completions_impl` with Chat JSON, because
that keeps both request and output ownership in the old machinery.

The proposed `generation_port` accepts a normalized internal request containing
prompt messages/content, sampling controls, grammar/structured-output controls,
model selection, decoded media buffers, and cancellation context. It emits
typed events such as:

- text delta;
- reasoning delta;
- tool-call start, name, argument delta, and completion;
- usage update;
- terminal reason;
- structured error.

Tool-call events need a stable per-call key so interleaved parallel calls do not
share scalar stream state. The port does not assign OpenAI Responses item IDs,
sequence numbers, or JSON envelopes. Those belong to the Responses service.
Conversely, the Responses service does not schedule slots, tokenize prompts, or
sample tokens. Those belong to the existing server runtime.

The first adapter may be a careful extraction from
`handle_completions_impl`/the existing response reader. Once it is neutral,
Chat Completions and Responses can both consume it. Sharing at this layer is
valuable; sharing JSON protocol state machines is not.

### Typed Responses domain

The new module owns C++ representations for:

- requests and request options;
- input and output items;
- typed text, image, audio, video, and file content;
- reasoning and compaction items;
- function, custom, shell, search, MCP, computer, and image-generation calls;
- tool results, including multimodal results;
- response snapshots, statuses, errors, usage, and incomplete details;
- streaming events and cursor/sequence state.

Parsing is strict where OpenAI is strict. Any deliberate local extension is
namespaced and tested separately. Lowering typed input to `common_chat_msg`, a
model prompt, and media buffers happens in an inference adapter after protocol
validation and item/reference resolution.

Output moves in the opposite direction: neutral generation events are consumed
by a Responses state machine which is the only code allowed to assign Responses
item IDs, output indices, content indices, and sequence numbers.

### IDs and correlation

OpenAI item identity and tool-call correlation are different concepts. They
must never be represented by one overloaded field.

- Function-call output item ID: `fc_*`.
- Function-call correlation ID: `call_*`.
- Custom tool call and output IDs retain their distinct families, such as
  `ctc_*` and `ctco_*`, when required by the contract.
- Search and other hosted call/output item IDs likewise keep their own item
  families.

An existing well-formed ID supplied in replayed history is preserved. Newly
generated IDs are created once and remain stable across `output_item.added`,
argument deltas, `output_item.done`, the final response, stored input items, and
the next request's tool output. Failure and cancellation events reuse the same
response ID and sequence stream; they do not create a replacement response.

### Response storage

State is a first-class service, not an item LRU attached to request conversion.
The `response_store` interface has both a bounded in-memory test implementation
and the production SQLite implementation.
The interface currently owns atomic response snapshots, original input items,
generated output items, status transitions, metadata, pagination, and
compare-and-swap revisions. Expiry, canonical event history, active ownership,
and response-to-router recovery remain extensions of that contract.

Required semantics include:

- `store` policy;
- `previous_response_id` continuation;
- `item_reference` resolution;
- retrieve, delete, and list-input-items;
- background state transitions and cancellation;
- compacted context and conversation attachment;
- bounded memory and deterministic eviction errors.

Production foreground resources persist in
`~/.cache/llama.cpp/responses.sqlite3`; `LLAMA_RESPONSES_DB` can override the
complete path. The versioned schema stores canonical snapshots, continuation
lineage, detached descendant context, and item indices transactionally. It does
not yet persist active event history, background scheduling, expiry, or router
ownership. Those additions must not change the route or orchestration API.

### Hosted tools

Hosted tools use a C++ strategy interface and registry. A provider advertises
typed capabilities, validates/prepares a request, executes with cancellation,
emits progress, and returns typed text/media/file content or a structured error.

The orchestration loop is owned by the Responses service:

```text
inference -> hosted call item -> provider -> hosted result item -> inference
          -> repeat until terminal, cancelled, or policy limit
```

Execution ownership is explicit:

- Function and custom tools are normally client-executed; the response ends
  with call items and the client returns correlated output items.
- OpenAI-hosted tool types are server-executed only when a matching provider is
  configured.
- Local shell and similar extensions can be client-executed or mapped to a
  configured server provider by policy; the API layer never secretly spawns a
  shell.

Initial providers are fail-closed stubs. Later implementations may use MCP
stdio/Streamable HTTP, direct native code, ordinary HTTP services, supervised
subprocesses, or a TypeScript bridge. Transport selection is dependency
injection, not API branching.

Provider policy also owns timeouts, output limits, approval gates, working
directories, network/filesystem permission, maximum tool rounds, and audit
metadata. No provider receives ambient authority merely because it was
registered.

## Current compatibility baseline

The fork already has valuable behavior which must become regression coverage:

- string and item-array input, instructions, developer/system folding, and
  multi-turn assistant replay;
- text, refusal, reasoning, function/custom/local-shell/tool-search/web/file/
  image call shapes used by Codex;
- multimodal tool outputs containing text, images, files, and video data;
- compaction summaries and encrypted/opaque reasoning snapshots where present;
- item-reference caching;
- synchronous and streaming response envelopes, usage, indices, and telemetry;
- `/v1/responses/input_tokens`;
- `apply_patch` and `update_plan` custom-tool wire behavior used by Codex. A
  real Codex run advertises the dedicated `apply_patch` tool only when its
  model catalog says the model supports it; see the client-metadata section
  below;
- Codex namespace containers for installed app/plugin tools. The boundary
  validates nested client-executed function/custom declarations, lowers them
  to collision-safe Chat-template names, and restores the namespace on
  generated call items and replay.

Remaining areas to correct or complete include:

- native per-tool streaming state for genuinely generated, interleaved parallel
  calls;
- stable active-state snapshots, initial null usage, cancellation identity, and
  exact event ordering;
- the rest of the create-field and request-dependent envelope matrix;
- file resolution rather than graceful-but-lossy placeholders;
- strict validation and OpenAI-compatible errors for every unknown or
  unsupported field and content variant;
- cancel, compact, active event history, and background resource state;
- background mode, conversations, WebSocket/resume behavior, and router-aware
  state ownership;
- real hosted-tool execution.

The baseline is an asset, not proof that the current structure should be kept.

### Speculative decoding and logprobs side quest

On 2026-08-24 we audited llama.cpp
[PR #27196](https://github.com/ggml-org/llama.cpp/pull/27196), which remains open
and unmerged. It fixes missing probability population for tokens accepted by
the shared speculative-decoding path. That path is also used by
`--spec-type draft-mtp`: without the patch, the first ordinary token has real
probabilities, while later accepted tokens can report `logprob: 0` and an empty
`top_logprobs` array. We manually carried the PR's minimal target-logit-index
fix and added a speculative regression. `post_sampling_probs` under speculative
decoding remains unsupported.

This is not the same defect as Responses `top_logprobs: 0`. The latter was a
valid Responses default leaking into the transitional Chat-shaped inference
lowering, where the presence of `top_logprobs` implies Chat `logprobs: true`.
The sidecar therefore still removes the zero-valued field only from the private
forwarded request while retaining it in the public Responses object. PR #27196
is immediately useful for native completion logprobs under MTP and removes a
future blocker for nonzero Responses logprobs; it cannot replace that boundary
normalization or the Responses error contract.

## Implementation checkpoint: 2026-08-24

The current pass has a statically linked `tools/llama-responses/` module, an
explicit route-bundle factory, typed IDs and response snapshots, provider
strategy interfaces with fail-closed stubs, and route handlers for create,
input-token counting, retrieve, delete, and input-item pagination. Production
resources use a versioned SQLite store in the llama.cpp cache. Stored terminal
responses, item indices, and continuation context survive process restart;
deleting a parent transactionally detaches enough context for an existing
child to survive another restart and produce a grandchild.

Every stored response also owns its fully materialized input snapshot. A
continuation therefore needs one parent read rather than a live ancestor walk:
deleting an interior ancestor, or deleting the parent after generation starts
but before the child is committed, cannot strand the child's future lineage.
For `stream: true, store: true`, complete SSE frames are held until a terminal
snapshot commits; a storage failure replaces the would-be terminal with an
SDK-decodable `response_store_error` event before either HTTP or resumable
stream storage sees it.

The create path accepts string and typed-item inputs, string or typed-item-array
instructions, structured `text.format`, function/custom tool round trips, and
multimodal tool results. Sync and SSE responses share stable response, item,
and call identities. The server observes complete SSE frames even when an HTTP
write splits a frame, and response storage outlives asynchronous HTTP writes
without retaining dangling route pointers. A dated OpenAI oracle establishes
that `/input_items` materializes prior input, prior output, and current input;
the implementation and fixture now follow that lineage exactly, including
resolved `item_reference` values.

Phase 3 now has a typed, cancellable generation contract, deterministic scripted
port, and native Responses state machine which owns IDs, output assembly,
lifecycle, sequence numbers, sync snapshots, and SSE projections in focused
tests. The real task/response-reader drain also exposes an optional neutral sink
for parsed deltas, tool metadata, usage, errors, cancellation, and terminal
state while retaining slots, MTMD, pings, reader cancellation RAII, and
resumable streams. The sidecar does not yet implement that sink or call the
internal `generate(request, sink)` hook. The legacy create route therefore
remains the production default and generic `server-task` code still owns the
bytes returned by real generation.

The request-policy layer now normalizes documented nullable defaults; validates
metadata and identifier character limits; handles max-output and incomplete
lifecycle correctly even with partial output; validates reasoning, stream,
context, text-format, client-tool, Codex `client_metadata`, and service-tier
shapes; and rejects recognized unavailable fields and hosted tools with
parameter-attributed OpenAI error envelopes. Codex telemetry is retained at the
Responses boundary and removed before the private Chat-shaped inference DTO.
Codex namespace tools are treated as client-executed containers, not as hosted
providers: nested declarations are validated and deterministically flattened
for current llama.cpp chat templates, with reversible metadata retained for
Responses output and replay.

These decisions are backed by cheap dated probes against the real OpenAI
endpoint as well as the local SDK suite. The matrix is intentionally incomplete
rather than silently permissive.

The first advertised profile deliberately fails closed for long-tail or
stateful features which are not implemented yet:

- `background`, conversation resources, cancel, and compact;
- streamed retrieval/resume and WebSocket transport;
- active-response/event-history persistence, expiry, and cross-process/router
  response ownership;
- hosted web/file/computer/MCP tool execution;
- projected retrieval and most `include` values. Codex's
  `reasoning.encrypted_content` projection is accepted as a documented no-op
  because local generation produces no opaque encrypted payload;
- some newer create-request fields and long-tail typed variants.

SQLite deliberately has no 30-day eviction machine in this phase. It also does
not yet impose a byte budget on inline media. Those are operational policies,
not prerequisites for honest foreground resource semantics.

## Codex CLI model-catalog compatibility

This is a Codex client contract, not part of the public OpenAI Models or
Responses APIs. Codex uses model metadata to choose instructions, tools,
context limits, modalities, and request options. Merely accepting Responses
requests is enough for a basic turn, but it is not enough for deterministic
Codex behavior.

The public Codex configuration supports custom providers with a `base_url` and
`wire_api = "responses"`. Codex also implements a private model-catalog
discovery contract which is documented by its source rather than by the OpenAI
Responses specification. We first investigated that contract in OpenAI Codex
commit `3ba0f711`, including the
[request construction](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/codex-api/src/endpoint/models.rs#L31),
[catalog shape](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/protocol/src/openai_models.rs#L683),
and [fallback constructor](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/models-manager/src/model_info.rs#L138-L215).
That commit is provenance, not a compatibility pin.

When its remote-refresh policy permits a network fetch, Codex sends this shape
for a provider whose base URL is
`http://127.0.0.1:8081/v1`:

```text
GET /v1/models?client_version=<opaque client value>
```

It expects `{"models":[ModelInfo,...]}`, not the public OpenAI
`{"object":"list","data":[...]}` shape. The tagged schema is a fixture for the
fields we deliberately emit, not a routing version lock: the endpoint is a
forward/backward-compatible JSON bag. The server never parses or compares the
version and tests release, prerelease, future, and opaque values. Compatible
field additions require no version-specific server branch.

### When Codex calls the route

The endpoint and the client's decision to call it are separate compatibility
seams. In the inspected client source, Codex refreshes remote model metadata
only when the retained auth manager currently uses the first-party Codex
backend or the custom provider
defines `auth.command`. An `env_key` does not independently satisfy that gate,
although an existing ChatGPT login can satisfy it for the same custom provider.
With neither qualifying auth source, Codex deliberately skips the network fetch
and uses cached/bundled candidates plus unknown-model fallback metadata; no
defect in `/v1/models` can make that client state call the route. A configured
`model_catalog_json` selects a static manager and bypasses remote discovery for
a different reason.

For a local provider, command-backed bearer authentication opts into the
refresh and sends `Authorization: Bearer <token>` on model and Responses
requests. The canonical profile exports one `LLAMA_API_KEY` from `~/.env` to
both the authenticated llama-server and Codex's token command. This policy is
verified against the inspected source's
[refresh gate](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/models-manager/src/manager.rs#L437)
and with installed clients under isolated/no-OpenAI-auth state: no command auth
produced the unknown-model warning without a model request, while command auth
fetched and cached this catalog. A separate capture with the machine's ChatGPT
login retained confirmed that first-party auth also opens the gate. Command
auth is used below because it is deterministic across operator login state.

`~/.codex` is shared by the standalone CLI, editor helpers, and the desktop
application, which may install different Codex versions and share catalog
cache state. A pinned-model process can select fallback metadata on a cold
cache before a successful refresh becomes visible; the next process then uses
the refreshed catalog. Record the resolved `codex` binary and its version in a
smoke transcript, and do not infer route failure from one cold-start warning.

### Implemented route chain

`tools/llama-responses` now installs a neutral decorator through
`server_route_extensions`. The decorator is structurally first only for the
registered `/v1/models` route and receives the fully configured llama-server
handler as `next`:

- `/models`, with or without query parameters, always uses llama-server's
  original handler;
- `/v1/models` without a nonempty `client_version` also delegates unchanged;
- any nonempty `client_version` is recognized, regardless of its value;
- recognized requests call `next` exactly once, then project the returned live,
  sleeping/cached, or router catalog into the private Codex shape;
- non-200, streaming, malformed, or unprojectable downstream responses are
  returned unchanged, including their status and headers.

This is a chain-of-responsibility seam. Additional harness-specific query
contracts can be inserted ahead of or behind it without adding query-policy
branches to generic llama-server code. Tests intentionally include release,
prerelease, future, and non-version `client_version` bags to prevent an
accidental exact-version gate.

The current projection uses the following policies:

| Codex field | Source or policy |
| --- | --- |
| `slug` | llama-server's model `id`, including the configured `--alias`. |
| `context_window`, `max_context_window` | Actual loaded `data[].meta.n_ctx`; omitted when an unloaded router entry cannot report it. The two values are equal so a Codex config override cannot exceed the runtime. |
| `input_modalities` | Router `architecture.input_modalities`, or `text` plus `image` for the known single-model Qwen+mmproj alias; otherwise explicit `text`. The generic single-model `multimodal` bit cannot distinguish image from audio, so the projection fails closed. |
| shell and patch tools | `shell_type = "unified_exec"` and `apply_patch_tool_type = "freeform"`; both shapes are already accepted by the Responses bridge. |
| hosted search | Not advertised; the C++ provider interfaces exist but the implementations are still stubs. |
| instructions | One inspectable Apache-2.0 [base-instructions file](tools/llama-responses/prompts/codex-base-instructions.md), initially imported from OpenAI Codex. It is a locally owned deployment baseline, not selected by `client_version` and not a prompt registry. Model-tuned changes remain future, eval-driven policy. |

### Qwen reasoning policy

The [Qwen3.8-27B model card](https://huggingface.co/Qwen/Qwen3.8-27B)
documents the native `reasoning_effort` values `low`, `medium`, and `xhigh`,
with `xhigh` as the model default. The local GGUF's embedded template agrees.
The Codex projection advertises those three values as `{effort, description}`
objects and sets `default_reasoning_level = "low"` for the exact
`qwen3.8-27b-local` alias.

The names travel through existing seams without another mapping layer:

```text
Codex supported_reasoning_levels selection
    -> Responses reasoning.effort
    -> llama-server reasoning_effort
    -> Qwen chat-template reasoning_effort
```

Thinking on/off is a separate dimension. llama-server accepts the API spelling
`reasoning_effort = "none"` by setting `enable_thinking = false`; `off` is not a
Codex reasoning-effort value, and `none` is not one of Qwen's native three, so
neither is advertised in the picker. Start this deployment with
`--reasoning auto --reasoning-effort low`: omitted requests default to low,
Codex sends the catalog default, explicit `medium`/`xhigh` requests override it,
and an explicit `none` request can still disable thinking.

The MVP declares `supports_reasoning_summary_parameter = false` and
`default_reasoning_summary = "none"`. The bridge can emit reasoning events, but
it does not yet implement OpenAI's selectable summary policy faithfully.

The model card describes a native 262,144-token context and text/image/video
inputs. The endpoint nevertheless advertises the loaded server's `n_ctx`, not
the theoretical maximum, and only modalities represented by Codex's enum and
the active llama-server deployment.

### What the fallback warning means

If remote catalog decoding fails or the requested slug is absent, Codex creates
client-side metadata for the unknown slug. stderr reports a model-refresh
decode error and an unknown-model warning; `codex exec --json` also emits a
non-terminal error item saying fallback metadata is in use. The turn continues
on `wire_api = "responses"`. This is not a fallback to Chat Completions and does
not mean `/v1/responses` failed.

At the source-provenance commit above, the important compiled fallback
differences were:

| Area | Client fallback | Consequence |
| --- | --- | --- |
| Instructions | The generic prompt from which this endpoint's current baseline was imported | Later clients may move independently; the endpoint owns one inspectable baseline rather than keying prompts by client version. |
| Context | 272,000 maximum, 95% effective budget, automatic compaction at 90% | Can be materially wrong for the loaded runtime. |
| Shell | `default`, resolving to UnifiedExec with stock features | Terminal tools remain available. |
| Patch | Dedicated `apply_patch` disabled | Codex exposes a different tool set. |
| Reasoning | No supported/default effort; summaries supported and default `auto` | Qwen's effort choice and summary policy are wrong. |
| Modalities | Defaults to text and image | Wrong for text-only deployments. |
| Tool output | 10,000-byte truncation | Independent of actual context and the endpoint's new 10,000-token policy. |

The initial prompt import is OpenAI Codex's Apache-2.0-licensed CLI fallback, not
evidence that every hosted OpenAI model catalog entry uses identical
instructions. Its source commit, checksum, license, and NOTICE are recorded in
the adjacent [prompt README](tools/llama-responses/prompts/README.md).

The fallback is useful but not capability-neutral. With command auth enabling
refresh and the decorator active, the canonical Qwen alias resolves from the
remote catalog after refresh. A cold pinned-model turn may still warn before
the refreshed cache is selected. A future Codex client that adds an actually
required field may fall back until the projection is extended; compatible JSON
additions require no server change and never justify an exact-version route.

### Codex namespace tool containers

Installed Codex apps/plugins can contribute top-level `namespace` tool
containers. They are not OpenAI-hosted tools: Codex remains responsible for
executing the nested client function/custom calls. On the 2026-08-24 live
smoke, the client sent nine namespaces containing 211 nested functions in
addition to ordinary function and custom tools.

Current Qwen chat templates accept only a flat tool list and cap function names
at 64 characters. The Responses boundary therefore validates each namespace,
deterministically lowers every `{namespace, name}` pair to a collision-safe
flat name, retains reversible metadata, restores the public namespace/name on
generated call items, and applies the same mapping to replayed calls. Focused
tests cover validation, collision handling, lowering, and replay. Forced
namespace `tool_choice`, deferred loading/tool search, and an end-to-end model
call of a nested connector remain explicit future coverage.

The complete installed catalog is large enough to be a deployment concern. It
tokenized to 86,729 prompt tokens in the live run; accepting namespace syntax
does not make those schemas free. The canonical server profile below allocates
90,112 tokens so the real request fits without hiding installed tools.

### What Ollama does

Ollama main at commit `f6c59d87` takes a different integration path. Its
[`ollama launch codex` guide](https://github.com/ollama/ollama/blob/f6c59d87038ae77f52d4adfbdc37363f8edd1ef3/docs/integrations/codex.mdx#L18-L77)
and
[`ensureCodexConfig` implementation](https://github.com/ollama/ollama/blob/f6c59d87038ae77f52d4adfbdc37363f8edd1ef3/cmd/launch/codex.go#L174-L229)
write a one-model `~/.codex/model.json`, point a dedicated Codex profile at it
with `model_catalog_json`, and launch Codex with that static catalog. Ollama's
[`/v1/models` handler](https://github.com/ollama/ollama/blob/f6c59d87038ae77f52d4adfbdc37363f8edd1ef3/server/routes.go#L1632-L1645)
continues returning the public OpenAI list shape and does not implement the
private `client_version` contract.

The generated
[`ModelInfo` entry](https://github.com/ollama/ollama/blob/f6c59d87038ae77f52d4adfbdc37363f8edd1ef3/cmd/launch/codex.go#L705-L753)
derives context and modalities, but hard-codes empty `base_instructions` and
reasoning levels. Ollama then lowers Responses `instructions` to the first
system message, while a model's Modelfile `SYSTEM` is only prepended when the
request has no system message. That is useful precedent for static client
configuration, not for endpoint-transparent discovery or prompt fidelity. Our
decorated HTTP route keeps the deployment policy in the server and works for
any compatible harness willing to request it.

## Delivery phases

Each phase ends with a gate. A phase is not complete merely because its code
compiles. `✅` marks a completed checkpoint, `🚧` active work, and `🕒` a later
phase with some foundations already in place. Checked items are evidence in the
tree; unchecked items remain obligations. The phases describe architectural
checkpoints, not a strict landing order.

### Phase 0: freeze the oracle and the specification 🚧

Purpose: make the current behavior and desired contract measurable before
moving code.

Completed work:

- [x] Build the fork with `ninja -j32` in `build/` and establish focused C++ and
  authenticated HTTP/SDK baselines.
- [x] Pin this roadmap to a dated OpenAI contract snapshot and preserve the
  useful fork behaviors as explicit compatibility requirements.
- [x] Capture deterministic sync and SSE fixtures for stable IDs, tool calls,
  structured output, and SDK decoding.
- [x] Complete PR archaeology without importing the rejected PR's architecture
  wholesale.

Outstanding work:

- [ ] Fill baseline fixture gaps for every behavior listed above, especially
  multimodal tool-result continuation and genuinely generated parallel calls.
- [ ] Capture the Codex CLI request/event subset with a deterministic fake model
  or scripted generation source.
- [ ] Finish the dated conformance matrix across request fields, item and event
  variants, resources, transitions, and error cases.
- [ ] Preserve additional PR-era fixtures only when they describe behavior we
  still want.

Gate:

- [x] The existing code builds.
- [x] The currently exercised behavior is green or explicitly documented as an
  existing failure.
- [ ] Every target feature has a conformance-matrix row and an owning phase.

### Phase 1: establish the sidecar and route seam ✅

Purpose: give fork-owned Responses code a stable home and one narrow injection
point inside the existing server lifecycle.

Completed:

- [x] Link `tools/llama-responses/` as a static library owned by
  `llama-server`.
- [x] Add a neutral route-extension bundle whose lifetime is explicit in
  `llama_server()`.
- [x] Install the Responses route factory after normal single-model or router
  handler construction, so the fully configured legacy routes are dependencies
  rather than hidden globals.
- [x] Keep the legacy create and input-token handlers as an injected adapter and
  behavioral oracle while the sidecar takes ownership incrementally.
- [x] Keep upstream-owned changes to a CMake edge, neutral declarations, and a
  small server construction hook.
- [x] Preserve existing CLI and non-Responses route behavior; focused route
  tests prove decorated and delegated paths.
- [x] Build successfully with `ninja -j32`.

Gate:

- [x] `llama-server` constructs and owns the sidecar through the neutral seam.
- [x] The existing Responses generator remains usable through dependency
  injection rather than direct sidecar knowledge of server internals.
- [x] The seam survives the exercised single-model, authenticated HTTP, and live
  Qwen paths.

### Phase 2: establish native protocol and resource foundations ✅

Purpose: move Responses identity, storage, resource policy, and provider
boundaries into the sidecar without waiting for native generation extraction.

Completed:

- [x] Add distinct typed response, item, and call IDs; response statuses; usage;
  errors; output items; and extensible event/snapshot types.
- [x] Add normalization, capture, and rendering codecs that preserve
  forward-compatible wire fields while overlaying typed authoritative state.
- [x] Add a thread-safe `response_store` interface and bounded in-memory
  implementation with revisions, item indexing, transition checks, and
  descendant-context preservation across deletion or eviction.
- [x] Add resource services for retrieve, delete, and input-item pagination with
  structured errors.
- [x] Add route orchestration for `store`,
  `previous_response_id`, continuation expansion, and `item_reference`
  resolution.
- [x] Add transport-neutral hosted-tool provider interfaces, capability
  reporting, progress/cancellation hooks, a registry, and fail-closed stubs.
- [x] Add stable `fc_*` versus `call_*` contract fixtures, SDK-decodable
  sync/SSE transcripts, split-frame storage coverage, and store/resource unit
  tests.
- [x] Keep generation behind the injected legacy adapter deliberately; Phase 2
  establishes native protocol/resource ownership, not native token-event
  production.
- [x] Build and pass the focused C++ and HTTP/SDK suites.

Gate:

- [x] The sidecar owns response identity, resource state, storage policy, and
  hosted-tool strategy contracts.
- [x] The legacy generator can feed the native resource layer without an HTTP
  Chat Completions hop.
- [x] Stored continuation, retrieval, deletion, pagination, streamed terminal
  capture, and item-reference behavior pass the current SDK suite.

### Phase 3: converge on the core OpenAI Responses resource 🚧

Purpose: finish the generation/event spine and move from the working
transitional endpoint to strict OpenAI HTTP semantics.

Completed work:

- [x] Implement stable function/custom call identity and validate interleaved
  tool-call ordering with SDK-decodable golden transcripts.
- [x] Implement durable foreground `store`, `previous_response_id`,
  `item_reference`, retrieve, delete, materialized input-item pagination, and
  normalized input-token counting over a versioned SQLite backend. Terminal
  snapshots, item indices, continuation context, and descendant detachment
  survive restart.
- [x] Capture a dated OpenAI `/input_items` oracle and implement the observed
  prior-input, prior-output, current-input lineage, including resolved
  `item_reference` values.
- [x] Preserve structured output and request-dependent wire fields through the
  native snapshot/codec layer.
- [x] Persist completed sync and split-frame SSE terminal responses without
  dangling route lifetimes. Stored streams release only complete frames and do
  not expose or cache a success terminal until SQLite commits it.
- [x] Make continuation snapshots self-contained, including middle-node
  deletion, restart, grandchild, and delete-during-generation races. Future
  continuation performs one parent snapshot read rather than walking live
  ancestors.
- [x] Reject the currently recognized unavailable stateful features—background,
  conversations, cancel, and compact—with structured errors.
- [x] Pass current synchronous and asynchronous OpenAI Python SDK coverage for
  foreground create, stream, continuation, retrieve, delete, input-item
  pagination, item references, input-token counting, and validation errors.
- [x] Define a typed generation request/session/port and typed cancellation,
  text, reasoning, function/custom-call, usage, and terminal updates, with a
  deterministic scripted implementation.
- [x] Implement one native Responses state machine which owns response/item/call
  IDs, output assembly, lifecycle, sequence numbers, synchronous snapshots, and
  SSE projections in focused scripted tests, including interleaved calls and
  completed/incomplete/failed/cancelled terminals.
- [x] Expose the real llama-server completion reader through an optional
  protocol-neutral generation sink carrying begin/progress, parsed message
  diffs plus tool metadata, usage, errors, cancellation, and terminal state.
  Keep the legacy renderer as the production default and preserve slots, MTMD,
  pings, reader cancellation RAII, and resumable stream plumbing.
- [x] Accept Codex namespace tool containers, lower nested declarations to
  deterministic flat chat-template names, and preserve reversible call/replay
  metadata.
- [x] Complete an authenticated installed-Codex + Qwen3.8-27B Responses turn.

Outstanding spine:

- [ ] Implement the `tools/llama-responses` adapter for
  `server_generation_sink`: translate parsed reader diffs into native text,
  reasoning, stable per-index tool-start, tool-delta, usage, and terminal
  updates, including raw-argument accumulation and semantic custom-tool deltas.
- [ ] Route the supported foreground profile through the injected internal
  `generate(request, sink)` hook so `native_response_state_machine` owns the
  actual synchronous body and SSE bytes; persist typed snapshots directly
  instead of recovering terminal state from legacy wire output.
- [ ] Differential-test native projection against the legacy oracle for sync,
  split-frame SSE, late errors, limits, cancellation, and genuinely generated
  interleaved tool calls.
- [ ] Move Responses event/envelope construction out of generic
  `server-task`/`server-context` code after the native path is proven.
- [ ] Lower references and multimodal tool results from the typed domain rather
  than through the transitional Chat-shaped request adapter.
- [ ] Make native generation the default for every advertised supported shape
  and retain legacy only as a short-lived test oracle/fallback.

Outstanding HTTP/resource work:

- [ ] Complete the create-request field matrix, including remaining `include`,
  prompt/cache, moderation/safety, parallel-tool, and request-echo policy.
- [ ] Implement exact production active-state transitions, incomplete details,
  usage, and error/event ordering for sync and streaming.
- [ ] Add foreground cancellation before advertising background mode; the
  neutral sink currently supplies only the polling and cancelled-update seam.
- [ ] Audit every accepted create field so unsupported behavior is rejected
  explicitly rather than ignored by the transitional adapter.
- [ ] Add remaining typed resource and content variants beyond the foreground
  SDK subset already covered synchronously and asynchronously.
- [ ] Connect native active snapshots to store compare-and-swap and persist a
  canonical event history; production SQLite currently captures terminal
  foreground state even though the backend supports revisions/transitions.
- [ ] Add expiry/GC, byte accounting and inline-media budgets, and operational
  retention policy without changing the resource API.

Gate:

- [ ] Native generation is the default and no Responses event construction
  remains in generic task code.
- [ ] The Phase 3 conformance matrix is green for all advertised HTTP
  capabilities.
- [ ] Official SDK objects round-trip without local patches across the complete
  advertised matrix.
- [ ] Stored continuation and deletion tests cover text, reasoning, tool calls,
  and multimodal tool outputs.
- [ ] Error fixtures cover malformed, unsupported, missing, expired,
  conflicting, and cancelled resources.

### Phase 3.5: advertise truthful Codex model capabilities 🚧

Purpose: make Codex choose the correct prompt, context, modalities, tools, and
reasoning policy without changing public model-list behavior.

Completed work:

- [x] Add a neutral chain-of-responsibility decorator for `/v1/models` after
  normal single-model or router handler selection.
- [x] Intercept any nonempty `client_version`, call the downstream handler
  once, and preserve absent/unrelated queries, downstream errors, malformed
  bodies, and already-private catalogs.
- [x] Leave `/models` and undecorated `/v1/models` responses unchanged.
- [x] Project live context, text/image modalities, Codex shell/patch tools, and
  Qwen `low`/`medium`/`xhigh` reasoning with a low default.
- [x] Embed one Apache-2.0 Codex-derived base-instructions file with import
  provenance, license, and NOTICE. It is intentionally independent of
  `client_version`.
- [x] Document Codex's auth/cache refresh gate and use one bearer token for both
  Codex command auth and llama-server authentication.
- [x] Verify unit, authenticated HTTP/SDK, and live Codex + Qwen behavior,
  including cold fallback followed by refreshed-catalog selection.

Outstanding work:

- [ ] Replace the Qwen-alias special case with configurable or model-derived
  capability policy for arbitrary local and router models.
- [ ] Add end-to-end fixtures for API prefixes, sleeping/cached models, and
  actual router-process handler substitution; router-shaped catalog projection
  already has focused unit coverage.
- [ ] Advertise reasoning summaries, verbosity, hosted search, and other Codex
  capabilities only as their server implementations become truthful.
- [ ] Capture Codex request/tool negotiation as a deterministic black-box
  fixture rather than relying only on the live smoke transcript.
- [ ] Track genuinely required catalog-field changes with tolerant fixtures;
  keep the one base-instructions policy independent of client versions.

Gate:

- [x] The canonical Qwen deployment is discovered after catalog refresh and
  completes an authenticated Codex Responses turn; a cold pinned-model turn is
  allowed to use client fallback once while refresh populates shared cache.
- [x] Public model-list clients observe the original llama-server response.
- [ ] Generic single-model, sleeping, prefixed, and router deployments have
  deterministic integration coverage.
- [ ] A non-Qwen deployment can advertise truthful capabilities without source
  changes.

### Phase 4: execute hosted tools through providers 🕒

Purpose: support the hosted-tool part of the real Responses contract without
coupling the API to one deployment.

Completed foundations:

- [x] Define transport-neutral provider contracts, capability reporting,
  progress/cancellation hooks, a registry, and fail-closed unavailable-provider
  stubs.
- [x] Keep provider transport independent of the public Responses item model;
  MCP is an adapter option, not the architecture.

Outstanding work:

- [ ] Implement execution policy and the hosted-tool inference-round loop.
- [ ] Start with MCP adapters for the locally available terminal and web-search
  services while keeping the interfaces transport-neutral.
- [ ] Add file-search and image-generation providers as separate strategies.
- [ ] Preserve typed media/file results through provider output, storage, prompt
  lowering, and streaming.
- [ ] Add deterministic fake providers for every provider contract test.
- [ ] Add timeouts, size limits, approval/permission hooks, maximum rounds, loop
  detection, and audit logging.

Gate:

- [ ] Hosted tool calls can drive multiple inference rounds inside one response.
- [ ] Cancellation interrupts both inference and provider work.
- [x] Missing providers fail closed with compatible errors.
- [ ] Provider failures never corrupt stored response state or event sequences.
- [ ] MCP is one passing adapter, not a special case in the protocol layer.

### Phase 5: background, compaction, conversations, and alternate transports 🕒

Purpose: finish the stateful and long-running portions of the API.

Completed foundations:

- [x] Define typed response statuses and enforce basic terminal transition and
  revision invariants in the store.
- [x] Reject background, conversations, cancel, and compact explicitly while
  their lifecycle semantics are unavailable.

Outstanding work:

- [ ] Implement background scheduling and atomic queued/in-progress/terminal
  state transitions.
- [ ] Complete cancel and retrieve behavior for active background responses.
- [ ] Implement compact using the same response/item store and context policy.
- [ ] Implement conversation attachment and any companion conversation
  resources required for a client to use the create contract normally.
- [ ] Implement WebSocket events, reconnection/resume semantics, and event
  cursors from the same canonical event log used by SSE.
- [ ] Make router mode retain or discover which child owns a response. Do not
  rely on random proxy selection for retrieve/cancel/delete.
- [ ] Extend the Phase 3 persistent backend for background scheduling, event
  cursors, router ownership, expiry, and restart recovery.

Gate:

- [ ] State-machine race tests cover cancel-versus-complete,
  delete-versus-retrieve, disconnect/reconnect, expiry, and router child loss.
- [ ] SSE and WebSocket views are projections of one canonical event history.
- [ ] Background and compact pass SDK-level tests.
- [ ] Router mode passes create/continue/retrieve/cancel/delete end to end.

### Phase 6: convergence and hardening 🕒

Purpose: earn the claim of production-quality OpenAI compatibility and make it
cheap to maintain.

Completed foundations:

- [x] Establish focused domain, server-conversion, authenticated HTTP/SDK, and
  live Qwen/Codex tests.
- [x] Adopt a touched-file clang-tidy policy and keep the new Responses-owned
  code clean under the repository configuration.
- [x] Keep the fork-owned server seam small enough to remain mechanically
  reviewable against upstream changes.
- [x] Run targeted dated probes against the real OpenAI endpoint for defaults,
  validation errors, max-output lifecycle, structured output, tool policy, and
  continuation lineage; commit the sanitized `/input_items` lineage fixture.

Outstanding work:

- [ ] Build an optional differential suite against the dated real OpenAI endpoint
  for schema, validation, errors, and event ordering. Sanitize and commit
  fixtures; normal development must not require credentials.
- [ ] Expand Codex black-box suites with scripted generation and live Qwen smoke
  tests beyond the single canonical turn.
- [ ] Fuzz request parsing, SSE framing, replayed items, provider output, and
  cancellation sequences.
- [ ] Soak parallel responses, storage eviction, disconnects, router churn, and
  hosted-tool loops.
- [ ] Audit authorization boundaries, SSRF/path traversal, subprocess handling,
  secret redaction, size limits, and denial-of-service surfaces.
- [ ] Remove the legacy Responses output path and compatibility-only special
  headers after the native path has been the sole default for a release window.
- [ ] Document supported, experimental, and unavailable capabilities and
  automate advancement of the OpenAI spec snapshot.

Gate:

- [ ] All advertised conformance rows pass across sync, SSE, and applicable
  WebSocket modes.
- [ ] Codex completes a broad tool/media/state test corpus against Qwen.
- [ ] No Responses JSON/event logic remains in generic sampling/task code.
- [ ] Replaying the fork's server seam after a representative upstream merge is a
  small, mechanical operation.

## Test strategy

The tests form a ladder; live-model tests are not a substitute for the lower
levels.

1. Domain unit tests: parsing, validation, IDs, state transitions, storage,
   pagination, event ordering, and provider policy.
2. Scripted-generation tests: exact sync objects and event streams without a
   model or GPU.
3. Golden baseline tests: protect all behavior currently supplied by this fork.
4. OpenAI conformance fixtures: dated schemas, accepted/rejected requests,
   errors, and event transcripts.
5. Official SDK integration: sync, async, streaming, retrieval, cancellation,
   pagination, and typed items.
6. Codex CLI black-box tests: function/custom/local-shell/search calls,
   `apply_patch`, `update_plan`, reasoning, compaction, parallel calls, and
   multimodal tool results.
7. Provider contract tests: deterministic fakes first, then MCP and other real
   adapters.
8. Live Qwen3.8-27B tests: prompt-template/tool-parser compatibility and
   end-to-end smoke/soak behavior.
9. Optional real-OpenAI differential runs: protocol evidence, never a required
   unit-test dependency.

Every bug should be reproduced at the lowest layer that can express it. Golden
fixtures must assert the whole event stream, not just concatenated text.

### Canonical live Qwen server

The live Codex/conformance profile runs on port 8081. This is intentionally
separate from Docker-hosted services on port 8080 and from the high ports used
by automated server tests. It uses the
[Qwen3.8-27B model](https://huggingface.co/Qwen/Qwen3.8-27B).

```bash
MODELPATH="$HOME/Devel/scripts-ken/gguf/unsloth/Qwen3.8-27B-GGUF"
MODEL="Qwen3.8-27B-UD-Q6_K.gguf"
MMPROJ="mmproj-BF16.gguf"
SERVER="$HOME/Devel/llama.cpp/build/bin/llama-server"

source "$HOME/.env"
: "${LLAMA_API_KEY:?LLAMA_API_KEY must be set in ~/.env}"

$SERVER \
    -m "$MODELPATH/$MODEL" \
    --mmproj "$MODELPATH/$MMPROJ" \
    --alias qwen3.8-27b-local \
    --port 8081 \
    --host 0.0.0.0 \
    --ctx-size 90112 \
    --reasoning auto \
    --reasoning-effort low \
    --temperature 1.0 \
    --top-p 0.95 \
    --top-k 20 \
    --min-p 0.0 \
    --presence-penalty 0.0 \
    --repeat-penalty 1.0 \
    -np 1 \
    -ngl -1 \
    --spec-type draft-mtp \
    --spec-draft-n-max 2 \
    --kv-unified \
    --image-max-tokens 4096 \
    --image-min-tokens 1024
```

No second Unix account or second Codex service is required. The CLI process can
be isolated from this Codex session with per-process provider configuration,
`--ignore-user-config`, and `--ephemeral`. With the server above running, this
is the canonical smoke command:

```bash
source "$HOME/.env"

env -u OPENAI_API_KEY -u OPENAI_BASE_URL \
    codex --ask-for-approval never exec \
    --ephemeral \
    --ignore-user-config \
    --ignore-rules \
    --strict-config \
    --sandbox read-only \
    --json \
    --color never \
    -C "$HOME/Devel/llama.cpp" \
    -m qwen3.8-27b-local \
    -c 'model_provider="llama_local_smoke"' \
    -c 'model_providers.llama_local_smoke.name="Local llama-server smoke"' \
    -c 'model_providers.llama_local_smoke.base_url="http://127.0.0.1:8081/v1"' \
    -c 'model_providers.llama_local_smoke.auth.command="/usr/bin/printenv"' \
    -c 'model_providers.llama_local_smoke.auth.args=["LLAMA_API_KEY"]' \
    -c 'model_providers.llama_local_smoke.requires_openai_auth=false' \
    -c 'model_providers.llama_local_smoke.wire_api="responses"' \
    -c 'check_for_update_on_startup=false' \
    -c 'web_search="disabled"' \
    -c 'agents.enabled=false' \
    -c 'mcp_servers={}' \
    -c 'analytics.enabled=false' \
    'Reply with exactly LOCAL_RESPONSES_OK and do nothing else.'
```

The launcher sources `~/.env`, which exports `LLAMA_API_KEY`; llama-server
consumes that supported environment variable directly and requires bearer
authentication on its protected routes. The Codex process inherits the same
variable, and its `auth.command` asks `/usr/bin/printenv` for the token without
putting the secret in this document or the process arguments. The inspected
Codex refresh policy treats command-backed provider auth as permission to
refresh remote model metadata; an `env_key` alone does not open that gate.

The command deliberately has no reasoning override or static
`model_catalog_json`: it exercises remote
`/v1/models?client_version=...` discovery, selects the catalog's low default,
and reports visibly if Codex selects compiled fallback metadata.

Verified live on 2026-08-24 with the `codex` resolved from `PATH` and the Qwen
profile above. That process resolved `/home/ken/.local/bin/codex`, which
reported `codex-cli 0.149.1`; separate desktop/editor helper binaries were also
installed but were not the smoke process. The loaded slot reported 90,112
context tokens; an unauthenticated catalog request returned 401; and the
authenticated catalog
returned the Qwen alias, loaded context, text/image modalities,
`low`/`medium`/`xhigh`, low default, and the shared base instructions. The first
cold pinned-model process warned that it selected unknown-model fallback while
also refreshing the catalog, yet completed exactly `LOCAL_RESPONSES_OK` through
`/v1/responses` (86,729 input tokens, including nine connector namespaces with
211 nested functions, and 39 output tokens). The second identical process used
the refreshed catalog with no warning, reused 86,725 cached prompt tokens, and
again completed exactly. This is an end-to-end compatibility observation, not
a performance or output-determinism contract.

## Upstream merge discipline

The fork earns its keep only if Responses can evolve without turning every
upstream merge into archaeology.

- New protocol, state, provider, and conformance code stays under
  `tools/llama-responses/`.
- Generic server code exposes neutral capabilities; it does not import
  Responses item/event vocabulary.
- Prefer one explicit route-construction hook over edits beside every route.
- Prefer one generation port over `TASK_RESPONSE_TYPE_OAI_RESP` branches spread
  through task processing.
- Reuse prompt rendering, model parsers, tokenization, media decoding, queues,
  slots, and response readers through neutral APIs.
- If shared code needs substantial Responses-specific conditionals, keep it in
  the module instead.
- Annotate copied behavior with its source function and upstream commit so it
  can later be compared or replaced.
- Keep a small seam test that fails when upstream changes the assumptions on
  which the module depends.

### Touched-file clang-tidy policy

Responses-owned code and any upstream server source or header intentionally
modified by this fork must be clean under the repository's `.clang-tidy`
configuration. Run clang-tidy against `build/compile_commands.json`, restricting
the header filter to headers this fork actually touched. Diagnostics in
untouched transitive upstream headers do not expand the cleanup boundary.

Prefer fixing a diagnostic. A `NOLINT`, `NOLINTNEXTLINE`, or scoped suppression
is acceptable only when the warning is inapplicable because of a concrete
invariant. Put a comment immediately above the suppression explaining that
invariant and why the obvious refactor would be worse or cannot provide useful
recovery. Bare suppressions are not allowed.

Responses-specific server conversion tests belong in
`test-llama-responses-server-chat`, not the large upstream `test-chat` target.
This keeps the fork's test ownership and tidy boundary explicit.

## Open questions to answer with spikes, not guesses

These do not change the architectural decision, but they affect implementation
details:

- Which supported request/tool subset should first select the optional native
  generation sink, and what explicit capability predicate prevents a silent
  semantic downgrade to or from the legacy oracle?
- Should response ownership in router mode be encoded in IDs, stored in the
  router, or both?
- Which create fields are meaningful for local inference and which require an
  explicit unsupported-capability response?
- What canonical event-log schema and active ownership metadata should augment
  the existing versioned SQLite snapshot store for cancellation, resume, and
  router recovery?
- Which OpenAI WebSocket/resume behavior belongs in the first conformance
  snapshot?
- Where should model-specific opaque reasoning state live so that replay is
  correct but protocol code remains model-neutral?
- Which hosted tools require explicit user approval, and how is that policy
  configured without putting deployment concerns in the request parser?

Each spike should end in a small test or an ADR amendment in this document.

## Next implementation slice

The neutral generation foundation, real-reader sink, terminal SQLite backend,
and continuation-lineage oracle now exist. The next slice is the sidecar-owned
production adapter, not another seam or storage spike.

Recommended next checkpoint:

1. Implement `server_generation_sink` in `tools/llama-responses` and translate
   real parsed reader updates into the native state machine.
2. Differential-test the adapter against the legacy oracle for sync/SSE,
   function/custom interleaving, limits, errors, and cancellation.
3. Route only the proven foreground capability subset through the internal
   `generate(request, sink)` hook; keep legacy as explicit oracle/fallback.
4. Register response identity before task submission, poll one atomic cancel
   flag from the owning reader thread, and persist native active/terminal state
   with compare-and-swap.
5. Flip native generation to the default only after SDK and live-Qwen smoke
   coverage proves that the advertised profile no longer depends on legacy
   Responses rendering.

Lower-risk conformance work can accompany that checkpoint without obscuring
it: add an inline-media byte budget; continue the accepted/unsupported create
field audit; and turn live Codex catalog negotiation into a deterministic
fixture. Generic model-policy and router/prefix/sleep catalog coverage are the
next natural Phase 3.5 work, but they are not a substitute for production
Responses state-machine ownership.
