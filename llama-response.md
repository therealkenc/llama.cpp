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

## Codex CLI 0.148 third-party model compatibility

This is a dated Codex client contract, not part of the OpenAI Responses API.
Codex 0.148 uses model metadata to decide which instructions, tools, context
limits, modalities, and request options to use. Merely accepting Responses
requests is enough for a basic turn, but it is not enough for deterministic
Codex behavior.

The public Codex configuration supports custom providers with a `base_url` and
`wire_api = "responses"`. In addition, this client version implements a private
model-catalog discovery contract which is documented by its tagged source, not
by the OpenAI Responses specification. See
[custom model providers](https://learn.chatgpt.com/docs/config-file/config-advanced#custom-model-providers),
the [0.148 model endpoint implementation](https://github.com/openai/codex/blob/rust-v0.148.0/codex-rs/model-provider/src/models_endpoint.rs),
and the [0.148 fallback constructor](https://github.com/openai/codex/blob/rust-v0.148.0/codex-rs/models-manager/src/model_info.rs#L138-L215).

### What the metadata warning means

For a provider whose base URL is `http://127.0.0.1:8081/v1`, Codex 0.148 sends:

```text
GET /v1/models?client_version=0.148.0
```

For capability discovery it expects a private object shaped as
`{"models":[ModelInfo,...]}`. It does not consume the standard OpenAI
`{"object":"list","data":[...]}` entries for this purpose. `llama-server`
currently returns both an Ollama-shaped top-level `models[]` and an OpenAI-style
`data[]`. Codex tries to decode the former as `ModelInfo` and fails first because
the entry has no `slug`; the useful context and modality fields under
`data[].meta` do not rescue that decode.

Two related diagnostics are therefore visible:

- stderr reports that `/models` refresh failed, including the first missing or
  invalid private-catalog field;
- stderr warns that the requested model is unknown, and `codex exec --json`
  emits a non-terminal error item saying fallback metadata is being used.

The turn continues. This is a warning about client-side model capability
selection, not a failed Responses request and not a fallback from Responses to
Chat Completions. Codex remains on `wire_api = "responses"`.

Adding only `slug` is not a fix. A minimally decodable nonempty 0.148 entry also
needs the following fields (and a useful entry needs accurate optional
capabilities as well):

```json
{
  "models": [{
    "slug": "qwen3.8-27b-local",
    "display_name": "qwen3.8-27b-local",
    "base_instructions": "<real Codex instruction prompt>",
    "supported_reasoning_levels": [],
    "shell_type": "default",
    "visibility": "none",
    "supported_in_api": true,
    "priority": 99,
    "support_verbosity": false,
    "truncation_policy": {"mode": "bytes", "limit": 10000},
    "experimental_supported_tools": []
  }]
}
```

An empty `base_instructions` value merely suppresses the good compiled fallback
prompt. A server-owned catalog must contain deliberate instructions or a
`model_messages.instructions_template`, plus truthful context, modality,
reasoning, and tool declarations.

### What Codex falls back from, and what it falls back to

Codex first looks for the requested slug in the resolved model catalog. If
remote decoding fails or the slug is absent, 0.148 constructs an in-client
`ModelInfo` for that unknown slug. For `qwen3.8-27b-local`, its relevant defaults
are:

| Area | Compiled fallback | Consequence |
| --- | --- | --- |
| Instructions | Codex's generic coding-agent base prompt | Good enough to operate, but selected by client version rather than server/model capability. |
| Context | 272,000 declared maximum, 95% effective budget (258,400), auto-compact at 244,800 | May materially overstate or understate the loaded server context; Codex ignores `data[].meta.n_ctx` in the current response. |
| Shell | `shell_type = default`, which resolves to UnifiedExec with stock 0.148 features | `exec_command`/`write_stdin` work. |
| Patch tool | `apply_patch_tool_type = null` | No dedicated `apply_patch` tool is advertised, so a black-box patch test does not cover that server shape. |
| Reasoning | no supported/default effort; reasoning-summary parameter supported and defaults to `auto` | Without an override, Codex sends `reasoning.summary = "auto"` even when this server is launched with `--reasoning off`. |
| Verbosity | unsupported | A requested model verbosity is not sent as a supported model feature. |
| Modalities | text and image; original image detail unsupported | Fits the Qwen+mmproj profile in broad shape, but is wrong for a text-only deployment. |
| Search/API mode | text web-search type, model search support false, Responses Lite false, direct tool mode | No model-native search capability is inferred; normal Responses requests are still used. |
| Tool output | byte truncation at 10,000 bytes | Long tool output is retained only up to roughly 2,500 tokens, independent of server limits. |
| Model-specific guidance | skill, plugin, and app usage-instruction flags false; no experimental tools | Those catalog-gated instruction blocks and tools are not enabled. |

The fallback is therefore useful but not capability-neutral. A third-party
provider can have a perfectly usable `/v1/responses` endpoint and still produce
this warning, different tools, a wrong context policy, or unsuitable reasoning
options if its `/models` endpoint does not implement Codex's versioned private
catalog.

### Three supported operating choices

1. During an exploratory smoke test, accept the warning and override known
   mismatches explicitly. For the canonical `--reasoning off` profile, set
   `model_reasoning_effort = "none"` and `model_reasoning_summary = "none"`.
2. For deterministic tests, set
   `model_catalog_json = "/absolute/path/to/codex-qwen-models.json"`. In 0.148,
   a valid nonempty static catalog is authoritative and bypasses remote
   `/models` discovery. It should carry the real instructions and advertise
   the actual context, modalities, patch-tool type, truncation, and reasoning
   policy.
3. In a later sidecar iteration, add one optional model-metadata decorator seam.
   Generic `/v1/models` remains owned by `llama-server`; the statically linked
   Responses module may add a versioned Codex catalog projection for active,
   sleeping/cached, and router model lists. This avoids scattering a private
   Codex schema and prompt through upstream-owned server code.

The configuration key `model_reasoning_summary` is accepted by Codex 0.148.
The similarly named `model_supports_reasoning_summaries` is not a recognized
0.148 configuration key. The private catalog must be versioned as a client
compatibility fixture; it must not become the source of truth for the OpenAI
Responses contract.

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
by automated server tests.

```bash
MODELPATH="$HOME/Devel/scripts-ken/gguf/unsloth/Qwen3.8-27B-GGUF"
MODEL="Qwen3.8-27B-UD-Q6_K.gguf"
MMPROJ="mmproj-BF16.gguf"
SERVER="$HOME/Devel/llama.cpp/build/bin/llama-server"

$SERVER \
    -m "$MODELPATH/$MODEL" \
    --mmproj "$MODELPATH/$MMPROJ" \
    --alias qwen3.8-27b-local \
    --port 8081 \
    --host 0.0.0.0 \
    --reasoning off \
    --temperature 0.7 \
    --top-p 0.80 \
    --top-k 20 \
    --min-p 0.0 \
    --presence-penalty 1.5 \
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
env -u OPENAI_API_KEY -u OPENAI_BASE_URL \
    LLAMA_RESPONSES_API_KEY=dummy \
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
    -c 'model_providers.llama_local_smoke.env_key="LLAMA_RESPONSES_API_KEY"' \
    -c 'model_providers.llama_local_smoke.requires_openai_auth=false' \
    -c 'model_providers.llama_local_smoke.wire_api="responses"' \
    -c 'model_reasoning_effort="none"' \
    -c 'model_reasoning_summary="none"' \
    -c 'check_for_update_on_startup=false' \
    -c 'web_search="disabled"' \
    -c 'agents.enabled=false' \
    -c 'mcp_servers={}' \
    -c 'analytics.enabled=false' \
    'Reply with exactly LOCAL_RESPONSES_OK and do nothing else.'
```

`LLAMA_RESPONSES_API_KEY` is deliberately a dummy value: Codex's provider
configuration names an environment key, while this local server profile does
not require OpenAI authentication. When a static catalog is ready, add
`-c 'model_catalog_json="/absolute/path/to/codex-qwen-models.json"'`; until
then the unknown-model fallback warning described above is expected.

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
5. Add a deterministic Codex 0.148 catalog fixture or the optional model-list
   decorator seam so dedicated `apply_patch`, actual context limits, and
   reasoning policy are tested deliberately.
6. Put a byte budget around stored inline media before advertising the
   in-memory store beyond development use.
7. Continue Phase 3 field/error coverage, then begin real hosted providers as
   Phase 4 rather than embedding transport choices in the API layer.

The central remaining question is now concrete: how small the generation seam
can be while preserving the mature slot, parser, chat-template, and media
runtime and letting `tools/llama-responses` become the sole owner of Responses
items and events.
