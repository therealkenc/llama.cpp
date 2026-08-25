# A first-class OpenAI Responses API in `llama-server`

Status: phases 1--3 first implementation pass in progress
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
- `POST /v1/responses/{response_id}/compact`;
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
Define a `response_store` interface with an in-memory implementation first.
The interface owns atomic response snapshots, original input items, generated
output items, status transitions, metadata, expiration, pagination, and
response-to-router ownership.

Required semantics include:

- `store` policy;
- `previous_response_id` continuation;
- `item_reference` resolution;
- retrieve, delete, and list-input-items;
- background state transitions and cancellation;
- compacted context and conversation attachment;
- bounded memory and deterministic eviction errors.

Persistence across process restart is a separate backend decision. The API must
not bake an in-memory assumption into its domain types; an optional SQLite or
external store can be added without changing route or orchestration code.

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
  below.

Known areas to correct or complete include:

- independent item IDs and call IDs;
- per-tool streaming state for interleaved parallel calls;
- stable `created_at`, initial null usage, and request-dependent envelope fields;
- structured `text.format` preservation;
- exact failure/cancellation identity and event ordering;
- complete `store`/`previous_response_id` behavior;
- file resolution rather than graceful-but-lossy placeholders;
- strict validation and OpenAI-compatible errors instead of skipping unknown or
  unsupported content;
- retrieve, delete, cancel, compact, and list-input-items resources;
- background mode, conversations, WebSocket/resume behavior, and router-aware
  state ownership;
- real hosted-tool execution.

The baseline is an asset, not proof that the current structure should be kept.

## Implementation checkpoint: 2026-08-24

The first pass now has a statically linked `tools/llama-responses/` module, an
explicit route-bundle factory, typed IDs and response snapshots, a bounded
in-memory response store, provider strategy interfaces with fail-closed stubs,
and route handlers for create, input-token counting, retrieve, delete, and
input-item pagination. Continuation and `item_reference` resolution honor
`store`, survive deletion correctly, and preserve prompt-visible descendant
context when a bounded store evicts an ancestor.

The create path accepts string and typed-item inputs, string or typed-item-array
instructions, structured `text.format`, function/custom tool round trips, and
multimodal tool results. Sync and SSE responses share stable response, item,
and call identities. The server observes complete SSE frames even when an HTTP
write splits a frame, and response storage outlives asynchronous HTTP writes
without retaining dangling route pointers.

This is not yet the final Phase 2 architecture. Generation still passes through
the existing in-process server task machinery and its Chat-shaped adapter as a
behavioral oracle. There is no HTTP call to Chat Completions, and the public
wire contract remains Responses, but the neutral generation port and native
event-owner extraction are still the largest structural work left. The module
already owns request normalization, resource state, storage policy, and route
orchestration; generic `server-task` code still owns too much Responses event
construction.

The first advertised profile deliberately fails closed for long-tail or
stateful features which are not implemented yet:

- `background`, conversations, cancel, and compact;
- streamed retrieval/resume and WebSocket transport;
- durable storage, expiry, and cross-process/router response ownership;
- hosted web/file/computer/MCP tool execution;
- all `include` projections and some newer create-request policy fields.

The in-memory store is bounded by response count, not bytes. Inline media can
therefore consume substantial memory even below the entry cap. `/input_items`
currently exposes the items supplied for the current response rather than a
materialized copy of its whole continuation lineage; this needs an OpenAI
oracle fixture before changing it.

## Codex CLI model-catalog compatibility

This is a Codex client contract, not part of the public OpenAI Models or
Responses APIs. Codex uses model metadata to choose instructions, tools,
context limits, modalities, and request options. Merely accepting Responses
requests is enough for a basic turn, but it is not enough for deterministic
Codex behavior.

The public Codex configuration supports custom providers with a `base_url` and
`wire_api = "responses"`. Codex also implements a private model-catalog
discovery contract which is documented by its source rather than by the OpenAI
Responses specification. The first implementation is pinned against Codex
0.148, commit `3ba0f711`, including the
[request construction](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/codex-api/src/endpoint/models.rs#L31),
[catalog shape](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/protocol/src/openai_models.rs#L683),
and [fallback constructor](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/models-manager/src/model_info.rs#L138-L215).

When its remote-refresh policy permits a network fetch, Codex 0.148 sends the
following request for a provider whose base URL is
`http://127.0.0.1:8081/v1`:

```text
GET /v1/models?client_version=0.148.0
```

It expects `{"models":[ModelInfo,...]}`, not the public OpenAI
`{"object":"list","data":[...]}` shape. The tagged schema is a fixture for the
fields we deliberately emit, not a routing version lock: the endpoint is a
forward/backward-compatible JSON bag, and unknown fields are ignored by Codex.

### When Codex calls the route

The endpoint and the client's decision to call it are separate compatibility
seams. Codex 0.148 refreshes remote model metadata only when the retained auth
manager currently uses the first-party Codex backend or the custom provider
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
verified against Codex 0.148's
[refresh gate](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/models-manager/src/manager.rs#L437)
and with the installed CLI under isolated/no-OpenAI-auth state: no command auth
produced the unknown-model warning without a model request, while command auth
consumed this catalog and removed the warning. A separate capture with the
machine's ChatGPT login retained confirmed that first-party auth also opens the
gate. Command auth is used below because it is deterministic across operator
login and cache state.

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
branches to generic llama-server code. Tests intentionally include a future
`client_version=99.0.0` to prevent an accidental exact-version gate.

The current projection uses the following policies:

| Codex field | Source or policy |
| --- | --- |
| `slug` | llama-server's model `id`, including the configured `--alias`. |
| `context_window`, `max_context_window` | Actual loaded `data[].meta.n_ctx`; omitted when an unloaded router entry cannot report it. The two values are equal so a Codex config override cannot exceed the runtime. |
| `input_modalities` | Router `architecture.input_modalities`, or `text` plus `image` for the known single-model Qwen+mmproj alias; otherwise explicit `text`. The generic single-model `multimodal` bit cannot distinguish image from audio, so the projection fails closed. |
| shell and patch tools | `shell_type = "unified_exec"` and `apply_patch_tool_type = "freeform"`; both shapes are already accepted by the Responses bridge. |
| hosted search | Not advertised; the C++ provider interfaces exist but the implementations are still stubs. |
| instructions | The exact 20,903-byte Codex 0.148 fallback base prompt, embedded from the inspectable [prompt snapshot](tools/llama-responses/prompts/codex-0.148.0.md). `base_instructions` replaces the fallback rather than extending it, so matching the known baseline avoids an accidental prompt regression. Model-tuned prompts remain future, eval-driven policy. |

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

For Codex 0.148, the important compiled fallback differences are:

| Area | Client fallback | Consequence |
| --- | --- | --- |
| Instructions | The same generic 20,903-byte Codex 0.148 prompt embedded by this endpoint | Equivalent at this pinned client snapshot; the endpoint owns a stable, inspectable baseline instead of inheriting future client drift. |
| Context | 272,000 maximum, 95% effective budget, automatic compaction at 90% | Can be materially wrong for the loaded runtime. |
| Shell | `default`, resolving to UnifiedExec with stock features | Terminal tools remain available. |
| Patch | Dedicated `apply_patch` disabled | Codex exposes a different tool set. |
| Reasoning | No supported/default effort; summaries supported and default `auto` | Qwen's effort choice and summary policy are wrong. |
| Modalities | Defaults to text and image | Wrong for text-only deployments. |
| Tool output | 10,000-byte truncation | Independent of actual context and the endpoint's new 10,000-token policy. |

The prompt snapshot is OpenAI Codex's Apache-2.0-licensed CLI fallback, not
evidence that every hosted OpenAI model catalog entry uses identical
instructions. Its source commit, checksum, license, and NOTICE are recorded in
the adjacent [prompt README](tools/llama-responses/prompts/README.md).

The fallback is useful but not capability-neutral. With command auth enabling
refresh and the decorator active, the canonical Qwen alias resolves from the
remote catalog without this warning. A future Codex client that adds a required
field may fall back until we advance the schema fixture; accepting its
`client_version` is intentional, because compatible JSON additions require no
server change.

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
compiles.

### Phase 0: freeze the oracle and the specification

Purpose: make the current behavior and desired contract measurable before
moving code.

Work:

- Build the current fork with `ninja -j32` in `build/` and record the test
  baseline.
- Inventory current Responses tests and fill gaps for every behavior listed
  above, especially multimodal tool outputs.
- Capture deterministic non-streaming and SSE fixtures from the current handler.
- Capture the Codex CLI request/event subset with a deterministic fake model or
  scripted generation source.
- Create a dated conformance matrix from the official Responses schema: request
  fields, item variants, event variants, resources, status transitions, and
  error cases.
- Preserve selected PR-era fixtures only when they describe behavior we still
  want; do not import its architecture wholesale.

Gate:

- The existing code builds.
- Current behavior is green in CI or explicitly documented as an existing
  failure.
- Every target feature has a conformance-matrix row and an owning phase.

### Phase 1: introduce the two seams without changing behavior

Purpose: make Responses independently replaceable while keeping one server.

Work:

- Add `tools/llama-responses/` as a library linked into `llama-server`; do not
  add a new executable or listening socket.
- Introduce the Responses route-bundle factory and give it explicit lifetime in
  `llama_server()`.
- Extract the smallest viable neutral generation port from the current task/
  response-reader path.
- Implement a legacy adapter so the existing handler can run through the new
  route seam during comparison.
- Add typed request context, ID generation, cancellation context, response
  store interface, event sink, hosted-tool provider interface, registry, and
  unavailable-provider stubs.
- Make no public CLI changes unless the seam cannot be tested otherwise. Prefer
  construction-time injection in unit/integration tests over a permanent
  implementation-selection flag.
- Verify single-model and router-mode route registration.

Upstream-owned patch budget for this phase should be approximately:

- one CMake linkage change;
- a small route/factory declaration;
- a small hook in `server.cpp`;
- the neutral generation-port declaration and adapter;
- no broad Responses rewrite in `server-task.cpp`.

Gate:

- All existing endpoints and CLI behavior are unchanged.
- Legacy Responses output is fixture-equivalent through the new route seam.
- The new module can be substituted with a fake generation port in a unit test.
- `ninja -j32` succeeds.

### Phase 2: reach feature parity with a native Responses state machine

Purpose: replace Chat-JSON conversion and interleaved output construction while
losing none of the fork's present utility.

Work:

- Parse Requests directly into the typed Responses domain.
- Resolve references and media before lowering items to shared prompt/runtime
  structures.
- Port all existing item types, reasoning forms, compaction snapshots, Codex
  custom tools, and multimodal tool-result behavior.
- Build sync output and SSE events from the same state machine.
- Implement independent `fc_*` item IDs and `call_*` correlation IDs first, with
  stable replay behavior and tests.
- Track every parallel tool call independently by stable call key.
- Move Responses envelope/event construction out of generic generation tasks.
- Differential-test native versus legacy behavior, allowing only reviewed
  conformance corrections.
- Run Codex CLI against scripted output, then against Qwen3.8-27B.

Gate:

- The native implementation passes all baseline fixtures and Codex contract
  tests.
- Media tool outputs survive a complete call/output/continuation round trip.
- Parallel/interleaved tool streams are deterministic and correctly indexed.
- Native becomes the default; legacy remains only as a short-lived test oracle.
- `ninja -j32` succeeds.

### Phase 3: complete the core HTTP Responses resource

Purpose: move from fork parity to strict OpenAI resource semantics.

Work:

- Implement the complete create-request field matrix, including structured
  output, truncation/context policy, include fields, prompt/cache fields,
  moderation/safety identifiers where applicable, parallel-tool policy, and
  request-dependent echo fields.
- Implement exact response objects, status transitions, incomplete details,
  usage, and errors for sync and streaming paths.
- Implement real `store`, `previous_response_id`, and item-reference semantics.
- Add retrieve, delete, list-input-items with pagination, and exact input-token
  counting over the same normalization path used for generation.
- Add cancel for active foreground work even before background mode is exposed.
- Reject unsupported capabilities explicitly and compatibly.
- Run current OpenAI SDKs against the local endpoint, including typed event
  decoding and resource helpers.

Gate:

- The Phase 3 conformance matrix is green for all advertised HTTP capabilities.
- Official SDK objects round-trip without local patches.
- Stored continuation and deletion tests cover text, reasoning, tool calls, and
  multimodal tool outputs.
- Error fixtures cover malformed, unsupported, missing, expired, conflicting,
  and cancelled resources.

### Phase 4: execute hosted tools through providers

Purpose: support the hosted-tool part of the real Responses contract without
coupling the API to one deployment.

Work:

- Implement the provider registry, capability negotiation, execution policy,
  progress events, cancellation, and tool-round loop.
- Start with MCP adapters for the locally available terminal and web-search
  services, while keeping the interfaces transport-neutral.
- Add file-search and image-generation providers as separate strategies.
- Preserve typed media/file results through provider output, storage, prompt
  lowering, and streaming.
- Add deterministic fake providers for every provider contract test.
- Add timeouts, size limits, approval/permission hooks, maximum rounds, loop
  detection, and audit logging.

Gate:

- Hosted tool calls can drive multiple inference rounds inside one response.
- Cancellation interrupts both inference and provider work.
- Missing providers fail closed with compatible errors.
- Provider failures never corrupt stored response state or event sequences.
- MCP is one passing adapter, not a special case in the protocol layer.

### Phase 5: background, compaction, conversations, and alternate transports

Purpose: finish the stateful and long-running portions of the API.

Work:

- Implement background scheduling and atomic queued/in-progress/terminal state
  transitions.
- Complete cancel and retrieve behavior for active background responses.
- Implement compact using the same response/item store and context policy.
- Implement conversation attachment and any companion conversation resources
  required for a client to use the create contract normally.
- Implement WebSocket events, reconnection/resume semantics, and event cursors
  from the same canonical event log used by SSE.
- Make router mode retain or discover which child owns a response. Do not rely
  on random proxy selection for retrieve/cancel/delete.
- Decide and implement the first persistent `response_store` backend if process
  restart continuity is part of the advertised deployment profile.

Gate:

- State-machine race tests cover cancel-versus-complete, delete-versus-retrieve,
  disconnect/reconnect, expiry, and router child loss.
- SSE and WebSocket views are projections of one canonical event history.
- Background and compact pass SDK-level tests.
- Router mode passes create/continue/retrieve/cancel/delete end to end.

### Phase 6: convergence and hardening

Purpose: earn the claim of production-quality OpenAI compatibility and make it
cheap to maintain.

Work:

- Run an optional differential suite against the dated real OpenAI endpoint for
  schema, validation, errors, and event ordering. Sanitize and commit fixtures;
  normal development must not require credentials.
- Run Codex black-box suites with scripted generation and live Qwen smoke tests.
- Fuzz request parsing, SSE framing, replayed items, provider output, and
  cancellation sequences.
- Soak parallel responses, storage eviction, disconnects, router churn, and
  hosted-tool loops.
- Audit authorization boundaries, SSRF/path traversal, subprocess handling,
  secret redaction, size limits, and denial-of-service surfaces.
- Remove the legacy Responses output path and compatibility-only special headers
  after the native path has been the sole default for a release window.
- Document supported, experimental, and unavailable capabilities and automate
  advancement of the OpenAI spec snapshot.

Gate:

- All advertised conformance rows pass across sync, SSE, and applicable
  WebSocket modes.
- Codex completes a broad tool/media/state test corpus against Qwen.
- No Responses JSON/event logic remains in generic sampling/task code.
- Replaying the fork's server seam after a representative upstream merge is a
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
putting the secret in this document or the process arguments. Codex 0.148 also
treats command-backed provider auth as permission to refresh remote model
metadata; an `env_key` alone does not open that gate.

The command deliberately has no reasoning override or static
`model_catalog_json`: it exercises remote
`/v1/models?client_version=...` discovery, selects the catalog's low default,
and fails visibly if Codex falls back to compiled metadata.

Verified live on 2026-08-24 with Codex CLI 0.148 and the Qwen profile above:
the loaded slot reported 91,136 context tokens; an unauthenticated catalog
request returned 401; the authenticated catalog returned the Qwen alias,
text/image modalities, `low`/`medium`/`xhigh`, low default, and the exact prompt
checksum. The smoke command exited 0 with no unknown-model warning and produced
exactly `LOCAL_RESPONSES_OK` through `/v1/responses` (9,684 input tokens and 34
generated tokens). This is an end-to-end compatibility observation, not a
performance or output-determinism contract.

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

- What is the smallest generation-port extraction that supports typed,
  interleaved tool deltas without perturbing Chat Completions?
- Should response ownership in router mode be encoded in IDs, stored in the
  router, or both?
- Which create fields are meaningful for local inference and which require an
  explicit unsupported-capability response?
- Is in-process lifetime storage sufficient for the first advertised profile,
  or should SQLite land before background mode?
- Which OpenAI WebSocket/resume behavior belongs in the first conformance
  snapshot?
- Where should model-specific opaque reasoning state live so that replay is
  correct but protocol code remains model-neutral?
- Which hosted tools require explicit user approval, and how is that policy
  configured without putting deployment concerns in the request parser?

Each spike should end in a small test or an ADR amendment in this document.

## Next implementation slice

The route, domain, storage, ID, and provider foundations now exist. The next
slice should finish the architectural move rather than widening the transitional
adapter indefinitely:

1. Extract the neutral generation port and a scripted fake from the current
   task/response-reader path.
2. Move text, reasoning, and interleaved tool-call event ownership into one
   native Responses state machine, then differential-test it against the legacy
   oracle.
3. Add an exact split-frame/interleaved-stream fixture at the generation seam,
   including deferred web/file shell-wrapper identity.
4. Capture an OpenAI oracle fixture for continued-response `/input_items`
   semantics before changing the current-turn representation.
5. Exercise the implemented Codex catalog decorator with the real CLI, then
   capture its request and tool-set negotiation as a deterministic fixture.
6. Put a byte budget around stored inline media before advertising the
   in-memory store beyond development use.
7. Continue Phase 3 field/error coverage, then begin real hosted providers as
   Phase 4 rather than embedding transport choices in the API layer.

The central remaining question is now concrete: how small the generation seam
can be while preserving the mature slot, parser, chat-template, and media
runtime and letting `tools/llama-responses` become the sole owner of Responses
items and events.
