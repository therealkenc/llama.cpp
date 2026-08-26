# A first-class OpenAI Responses API in `llama-server`

Status: route/generation Cortés spine complete; Phase 3 conformance and Phase
3.5 generalization in progress
Spec snapshot: 2026-08-24

## Decision

We use dependency inversion to put a statically linked Responses
subsystem inside the existing `llama-server` executable.

The subsystem lives under `tools/llama-responses/` and builds as a library,
not as another executable. The existing `llama-server` remains responsible for
command-line parsing, process lifecycle, authentication, HTTP, model loading,
router mode, slots, metrics, and every non-Responses endpoint. The new subsystem
owns the OpenAI Responses protocol, state machine, resources, streaming
events, and hosted-tool orchestration.

In terms of the three choices:

1. Keep patching `llama-server`: this was the useful starting point, but it is
   no longer the installed sidecar's implementation or fallback. The stock
   upstream Responses route remains compiled and is selected when no extension
   is installed.
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
- Preserve upstream's stock behavior when no Responses extension is installed.
  Once installed, the sidecar owns every Responses route and must return an
  explicit error for unsupported behavior rather than falling back to a second
  protocol implementation.

## Why the original fork implementation was not the destination

The route inherited at the start of this work did not call Chat Completions over
HTTP. It did, however,
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

The pain was architectural rather than cosmetic. Background responses,
retrieval, cancellation, parallel hosted tools, resumable streaming, and real
conversation state would make those files more overloaded. Refactoring the
largest functions in place would still leave Responses protocol knowledge in
the inference engine and would continue to produce broad merge conflicts.
The installed sidecar path has now removed that transitional coupling; this
section records why the Cortés replacement was warranted.

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
 typed server_generation_service
   |
   v
 existing slots/task queue/model/chat-template runtime
```

There are two inversion seams.

### Route seam

`server_routes` already exposes assignable handlers, and router mode already
replaces them. The extension formalizes that pattern with a Responses route
bundle factory. `server.cpp` constructs the fork-owned implementation after the
normal server context exists and before HTTP routes are registered.

The route bundle is the ownership point for implemented and future handlers:

- `POST /v1/responses`;
- `POST /v1/responses/input_tokens`;
- `GET /v1/responses/{response_id}`;
- `DELETE /v1/responses/{response_id}`;
- `POST /v1/responses/{response_id}/cancel`;
- `POST /v1/responses/compact`;
- `GET /v1/responses/{response_id}/input_items`;
- streamed-retrieval and WebSocket entry points if those profiles are later
  selected.

Unversioned aliases may remain for compatibility, but `/v1` behavior is the
conformance target.

The implemented seam has these properties:

- construction is explicit and statically linked;
- handler lifetime is owned by `llama_server()`;
- no Responses JSON types leak into the generic HTTP/router layer;
- installing the extension supplies only the typed generation service, so a
  sidecar handler has no structural path back to stock Responses rendering;
- omitting the extension leaves upstream's stock Responses route selected;
- a model-serving process installs the sidecar. In llama-server's optional
  *router mode*, a front process dynamically starts model-serving children and
  proxies requests to the selected child. The router currently retains
  upstream create/count proxying and does not advertise the sidecar's stateful
  retrieve/cancel/delete routes.

### Generation seam

Responses uses a narrow, protocol-neutral way to submit inference work and
observe it. It does not call `handle_completions_impl` with Chat JSON, which
would keep both request and output ownership in the old machinery.

The implemented generation service accepts a normalized internal request
containing prompt messages/content, sampling controls,
grammar/structured-output controls, model selection, decoded media buffers,
and cancellation context. It emits typed events such as:

- text delta;
- reasoning delta;
- tool-call start, name, argument delta, and completion;
- usage update;
- terminal reason;
- structured error.

Tool-call events need a stable per-call key so interleaved parallel calls do not
share scalar stream state. The service does not assign OpenAI Responses item IDs,
sequence numbers, or JSON envelopes. Those belong to the Responses service.
Conversely, the Responses service does not schedule slots, tokenize prompts, or
sample tokens. Those belong to the existing server runtime.

The adapter reuses llama-server's prompt parser, media loading, chat-template,
slot, sampler, and response-reader machinery through a typed
`server_generation_service`. Create and input-token counting share the same
typed lowering boundary. The installed path never constructs a hidden HTTP or
Responses-to-Chat request. Inside the server-owned adapter, typed messages and
tools are encoded only far enough to reuse llama-server's established in-process
chat-template parser; the sidecar never owns that parser DTO and alone assigns
Responses IDs, state, and wire envelopes. Sharing at this inference layer is
valuable; sharing JSON protocol state machines is not.

### Typed Responses domain

The sidecar owns C++ representations for:

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
compare-and-swap revisions. In-process active generation ownership lives in the
route-lifetime registry rather than the durable store. Expiry, canonical event
history, and response-to-router recovery remain extensions of that contract.

`store: true` is the first-class durable profile, not a deferred feature. Its
SQLite response can be retrieved and deleted and supplies `/input_items`,
`previous_response_id`, and `item_reference` state. A synchronous foreground
`store: false` response is delivered without creating that resource, so the
client must replay the relevant history itself. A background `store: false`
response is the one necessary exception: it remains pollable while the
asynchronous job exists, without promising long-term retention. This matches
the API distinction described by OpenAI's
[conversation-state](https://developers.openai.com/api/docs/guides/conversation-state)
and [background-mode](https://developers.openai.com/api/docs/guides/background)
guides.

Required semantics include:

- `store` policy;
- `previous_response_id` continuation;
- `item_reference` resolution;
- retrieve, delete, and list-input-items;
- background state transitions and cancellation;
- compacted context and conversation attachment;
- bounded memory and deterministic eviction errors.

Production resources persist in `~/.cache/llama.cpp/responses.sqlite3`;
`LLAMA_RESPONSES_DB` can override the complete path. The versioned schema
stores canonical snapshots, continuation lineage, detached descendant context,
and item indices transactionally. A non-streaming background request is
immediately checkpointed as `in_progress` and currently uses SQLite even when
its response body truthfully echoes `store: false`, because polling requires a
resource after the create request returns. This is not a contract mismatch:
OpenAI likewise temporarily persists background `store: false` responses so
they can be polled. `store` controls the API's retention promise, not whether an
asynchronous implementation may write temporary bytes. This local deployment
therefore uses one SQLite backing for both values and deliberately has no
separate ephemeral store.

The schema does not persist a resumable event journal, expiry policy, or router
ownership. It also does not attempt to resume inference after process death.
Graceful shutdown cancels and joins owned jobs; on the next startup any durable
`queued` or `in_progress` snapshots left by a violent exit are atomically
changed to `failed` with error code `server_restarted`. Fresh requests are
independent of those terminal records. These policies must not change the
route or orchestration API if a richer deployment profile is added later.

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

The sidecar provides or protects these valuable behaviors, with fixture gaps
called out below:

- string and item-array input, instructions, developer/system folding, and
  multi-turn assistant replay;
- text, refusal, reasoning, function/custom/local-shell/tool-search/web/file/
  image call shapes used by Codex;
- multimodal tool outputs containing text, images, files, and video data;
- compaction summaries and encrypted/opaque reasoning snapshots where present;
- durable item-reference resolution;
- synchronous and streaming response envelopes, usage, and indices;
- Codex client metadata preservation, plus explicit unsupported errors for the
  two unmodeled llama progress/timing extensions when requested as `true`;
- `/v1/responses/input_tokens`;
- `apply_patch` and `update_plan` custom-tool wire behavior used by Codex. A
  real Codex run advertises the dedicated `apply_patch` tool only when its
  model catalog says the model supports it; see the client-metadata section
  below;
- Codex namespace containers for installed app/plugin tools. The boundary
  validates nested client-executed function/custom declarations, lowers them
  to collision-safe Chat-template names, and restores the namespace on
  generated call items and replay;
- active-response cancellation, including a foreground local extension, plus
  non-streaming background create, durable polling, cancellation, terminal
  delete, and an explicit conflict while deletion races active work.

Remaining areas to correct or complete include:

- request fields and response projections newly exposed by future Codex or SDK
  fixtures, classified as implemented, truthful no-ops, or explicit
  unsupported behavior;
- provider-backed `file_id` resolution rather than recovery placeholders;
- the four newer client-executed replay item families not typed by the current
  lowerer: `shell_call`, `shell_call_output`, `apply_patch_call`, and
  `apply_patch_call_output`;
- compact and conversations; resumable background streaming, WebSocket
  transport, and router-mode stateful resources only if those profiles become
  implementation priorities;
- real hosted-tool execution.

This behavior is a regression asset; the transitional structure which first
supplied it is no longer part of the installed sidecar path.

### Speculative decoding and logprobs side quest

On 2026-08-24 we audited llama.cpp
[PR #27196](https://github.com/ggml-org/llama.cpp/pull/27196), which remains open
and unmerged. It fixes missing probability population for tokens accepted by
the shared speculative-decoding path. That path is also used by
`--spec-type draft-mtp`: without the patch, the first ordinary token has real
probabilities, while later accepted tokens can report `logprob: 0` and an empty
`top_logprobs` array. We manually carried the PR's minimal target-logit-index
fix and added the additive
`tools/server/tests/unit/test_speculative_logprobs.py` regression so the stock
upstream test file remains untouched. `post_sampling_probs` under speculative
decoding remains unsupported.

This is not the same defect as Responses `top_logprobs: 0`. The latter was a
valid Responses default leaking into the transitional Chat-shaped inference
lowering, where the presence of `top_logprobs` implies Chat `logprobs: true`.
The typed lowerer therefore accepts the Responses default without forwarding an
unsupported zero-valued Chat option at all. PR #27196 is immediately useful for
native completion logprobs under MTP and removes a future blocker for nonzero
Responses logprobs; it cannot replace that boundary policy or the Responses
error contract.

## Implementation checkpoint: 2026-08-25

The current pass has a statically linked `tools/llama-responses/` module, an
explicit route-bundle factory, typed IDs and response snapshots, provider
strategy interfaces with fail-closed stubs, and route handlers for create,
input-token counting, retrieve, delete, cancel, and input-item pagination.
Production resources use a versioned SQLite store in the llama.cpp cache.
Stored terminal responses, item indices, and continuation context survive
process restart;
deleting a parent transactionally detaches enough context for an existing
child to survive another restart and produce a grandchild.

Every stored response also owns its fully materialized input snapshot. A
continuation therefore needs one parent read rather than a live ancestor walk:
deleting an interior ancestor, or deleting the parent after generation starts
but before the child is committed, cannot strand the child's future lineage.
For native `stream: true, store: true`, each complete SSE batch is released only
after its corresponding typed in-progress checkpoint commits, and a success
terminal is never exposed before the terminal CAS succeeds. A storage failure
emits an SDK-decodable `response_store_error` event and requests generation
cancellation.

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
lifecycle, sequence numbers, sync snapshots, and SSE projections. A concrete
sidecar adapter consumes the real task/response-reader's neutral parsed deltas,
tool metadata, usage, errors, cancellation, and canonical final-message
snapshot. It projects text, reasoning, function, custom, namespace, and local
shell calls directly into native items/events, including terminal parser
reconciliation and the model parser's raw-content recovery when parsing
produces no message.

Ordinary foreground creation now calls the injected typed
`generate(input, sink)` service. The sidecar lowers materialized Responses
items directly into a typed model-facing request: messages, call correlation,
tools, ordered media sources, structured output, and inference parameters do
not traverse a Chat Completions HTTP or Responses-to-Chat conversion seam.
llama-server retains ownership of media loading, chat-template rendering,
tokenization, slots, MTMD, sampling, and parsed generation updates. The native
state machine owns the actual sync body and SSE bytes, and typed
in-progress/terminal snapshots are written directly to SQLite with
compare-and-swap. A guarded CAS refresh accepts the one legitimate concurrent
mutation—ancestor deletion detaching context into an active child—only when
every generation-owned field still matches the sink's last committed state.
An active-response registry binds each live response ID to that same neutral
sink. The cancel route requests generation cancellation, waits for the sink's
terminal acknowledgement, and returns the canonical cancelled snapshot;
missing, repeated-cancel, already-terminal, and cancel-versus-complete outcomes
have deterministic coverage. OpenAI documents this route only for
`background: true`; accepting it for an active foreground response is a tested
local extension, not a claim about the official foreground contract.

Non-streaming `background: true` uses an owned, joinable worker and an owned
copy of the server request. Create first durably records and returns an
`in_progress` resource; retrieve polls that snapshot, cancel reaches the same
active sink, and delete returns 409 while work is active. The route bundle's
shutdown hook cancels and joins workers before llama-server tears down its task
queue. Public `background: true, stream: true` remains explicitly unsupported.
llama-server's existing task/slot queue is the admission scheduler; the
Responses layer does not duplicate it merely to expose a momentary `queued`
status. There is deliberately no event journal, GC, router-mode resource
ownership, or inference resurrection in this core. Startup instead
terminalizes active snapshots orphaned by a prior process death.

The installed route bundle receives only this typed generation service and
therefore cannot fall back to stock or fork-legacy Responses rendering. The
upstream stock route remains available only when no sidecar extension is
installed. The llama telemetry extensions `return_progress: true` and
`timings_per_token: true` are currently rejected with parameter-attributed
unsupported errors; `false` is accepted as a no-op. Supporting them later
requires neutral typed updates, not a hidden second renderer.

The typed lowerer preserves `call_id` independently from public output-item IDs
and carries mixed text/image/file tool results in prompt order. It resolves
already-materialized continuations and item references, accepts data-URI and
remote image sources, decodes text-file payloads, and fails explicitly when a
real provider-backed file reference or active-model media capability is absent.
Malformed or forward-unknown history instead receives visible recovery text;
it cannot silently erase a valid neighboring attachment. Inline media is
bounded to 32 MiB per request. A live authenticated Qwen3.8-27B+mmproj replay of
`text -> cat image -> text -> truck image` identified both images in order, and
the OpenAI Python SDK retrieved the stored rich function result with its
original `call_id` and content ordering intact.

The request-policy layer now normalizes documented nullable defaults; validates
metadata and identifier character limits; handles max-output and incomplete
lifecycle correctly even with partial output; validates reasoning, stream,
context, text-format, client-tool, Codex `client_metadata`, and service-tier
shapes; and rejects recognized unavailable fields and hosted tools with
parameter-attributed OpenAI error envelopes. `parallel_tool_calls: null`
normalizes to and echoes the documented `true` default. Omitted or null
`moderation` is accepted; any non-null value remains unsupported because the
public contract provides no documented disabled moderation object to accept as
a no-op. Codex metadata remains at the Responses boundary; only typed inference
fields cross the generation service.
Codex namespace tools are treated as client-executed containers, not as hosted
providers: nested declarations are validated and deterministically flattened
for current llama.cpp chat templates, with reversible metadata retained for
Responses output and replay.

These decisions are backed by cheap dated probes against the real OpenAI
endpoint as well as the local SDK suite. The matrix is intentionally incomplete
rather than silently permissive. A sanitized 2026-08-25 black-box fixture
captures the complete first create request and six tool declarations emitted by
standalone Codex 0.149.1, with dynamic fields normalized and every observed
top-level field classified by focused tests.

The advertised profiles deliberately fail closed for long-tail or stateful
features which are not implemented yet:

- conversation resources and compact;
- streaming background create;
- streamed retrieval/resume and WebSocket transport;
- active-response event replay, expiry, and router-mode stateful resource
  routing;
- hosted web/file/computer/MCP tool execution;
- unsupported retrieval projections. Repeated `include[]` query parameters are
  parsed and validated; seven documented projections are truthful
  materialized/no-ops, while persisted `reasoning.encrypted_content` is
  rejected because local storage has no encrypted payload to reveal. The same
  create projection remains a truthful no-op for Codex;
- some newer create-request fields and long-tail typed variants.

SQLite deliberately has no 30-day eviction machine in this phase. The request
boundary has an inline-media budget, but durable byte accounting, externalized
blob storage, and retention policy remain operational work rather than
prerequisites for the advertised resource semantics.

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
does not make those schemas free. The canonical live-test profile below uses
Q8 key/value caches and allocates 180,224 tokens so multi-round protocol tests
fit without hiding installed tools. This deliberately spends model quality to
buy test-harness context; output quality is not the subject of these runs.

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
- [x] Capture the standalone Codex 0.149.1 create request and tool negotiation
  as a dated, sanitized, deterministic fixture.
- [x] Cover multimodal tool-result continuation and genuinely generated
  parallel calls with deterministic and live evidence.

Outstanding work:

- [ ] Fill remaining baseline fixture gaps for behavior which a current client
  or conformance row actually exercises.
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
- [x] Install the Responses route factory after normal model-server handler
  construction. With no extension, the normal upstream handlers remain
  selected; with the sidecar installed, its route bundle owns Responses.
  Router mode retains upstream proxying until response ownership is designed.
- [x] Make the installed route contract depend only on a typed generation and
  token-count service. No stock or fork-legacy Responses handler is injected,
  so fallback is structurally impossible.
- [x] Restore every no-longer-needed fork edit to current upstream and keep the
  remaining overlap to the CMake edge, explicit route/generation declarations,
  route construction, and a neutral generation projection.
- [x] Preserve existing CLI and non-Responses route behavior; focused route
  tests prove decorated and delegated paths.
- [x] Build successfully with `ninja -j32`.

Gate:

- [x] `llama-server` constructs and owns the sidecar through the neutral seam.
- [x] The sidecar uses llama-server generation through dependency injection
  rather than direct knowledge of server internals.
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
- [x] Keep generation behind one injected service boundary; Phase 2 establishes
  native protocol/resource ownership independently of the concrete
  token-event adapter completed in Phase 3.
- [x] Build and pass the focused C++ and HTTP/SDK suites.

Gate:

- [x] The sidecar owns response identity, resource state, storage policy, and
  hosted-tool strategy contracts.
- [x] The injected generation boundary feeds the native resource layer without
  an HTTP Chat Completions hop.
- [x] Stored continuation, retrieval, deletion, pagination, streamed terminal
  capture, and item-reference behavior pass the current SDK suite.

### Phase 3: converge on the core OpenAI Responses resource 🚧

Purpose: keep the completed native generation/event spine and converge its
advertised HTTP resources on strict OpenAI semantics.

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
- [x] Reject conversation resources and compact while their lifecycle semantics
  remain unavailable. Independently reject streaming background create until
  resumable event history exists; non-streaming background lifecycle is
  implemented below.
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
- [x] Expose the real llama-server completion reader through a
  protocol-neutral generation sink carrying begin/progress, parsed message
  diffs plus tool metadata, usage, errors, cancellation, and terminal state.
  Preserve slots, MTMD, pings, reader cancellation RAII, and resumable stream
  plumbing, and emit an authoritative final parsed-message snapshot with the
  model parser's raw-content recovery when no structured message is produced.
- [x] Accept Codex namespace tool containers, lower nested declarations to
  deterministic flat chat-template names, and preserve reversible call/replay
  metadata.
- [x] Complete an authenticated installed-Codex + Qwen3.8-27B Responses turn.
- [x] Implement the concrete `tools/llama-responses` generation adapter for
  text, reasoning, stable per-index function/custom/local-shell calls, raw
  arguments, semantic custom-tool input, usage, terminal states, and canonical
  reconciliation.
- [x] Route the ordinary foreground profile through the injected typed
  `generate(input, sink)` service so the native state machine owns real sync
  and SSE output. Persist typed active and terminal snapshots directly with
  CAS.
- [x] Make native generation the production default for the supported
  foreground profile. The installed sidecar structurally owns the route and
  cannot select the stock renderer; unsupported telemetry extensions fail
  explicitly.
- [x] Give create and input-token counting one typed `server_generation_input`
  dependency-inversion seam. The sidecar lowers the fully materialized
  Responses domain directly into model-facing messages, tools, inference
  parameters, and ordered media coordinates; llama-server owns prompt
  rendering, counting, and generation.
- [x] Preserve `call_id` correlation and mixed text/image/file ordering across
  function/custom/computer tool results, resolved item references, SQLite
  restart, ancestor deletion, and detached continuation context. Unsupported
  real media fails explicitly, while malformed history remains visibly
  recoverable.
- [x] Enforce a 32 MiB aggregate inline-media request budget and prove the
  multiple-image tool-result path against authenticated Qwen3.8-27B+mmproj and
  the OpenAI Python SDK.
- [x] Lower references and multimodal tool results from the typed Responses
  domain rather than through the transitional Responses-to-Chat adapter.
- [x] Pass the focused neutral-sink, native-adapter, route, store, and resource
  tests and the current fork-owned Python Responses/Codex SDK suite;
  authenticated live sync/retrieve and multimodal replay; and installed-Codex
  text plus local-shell round trips. The three restored upstream stock-route
  tests deliberately assert its permissive eight-token budget and private
  telemetry extensions, so they are not sidecar conformance assertions.
- [x] Restore generic server sources to current upstream and remove the
  transitional fork Responses pipeline rather than extracting it. Stock
  upstream Responses remains the uninstalled default; an installed sidecar is
  given only the typed service and has no legacy handler to invoke.
- [x] Keep `return_progress: true` and `timings_per_token: true` honest with
  parameter-attributed unsupported errors until neutral progress/timing events
  exist; accept `false` without changing generation.
- [x] Register active foreground response IDs against their neutral generation
  sinks and implement cancellation with canonical cancelled snapshots, stable
  missing/repeated/terminal outcomes, and cancel-versus-complete coverage.
  Foreground cancellation is a local extension; the official cancel operation
  is documented only for responses created with `background: true`.
- [x] Implement the non-streaming background core with an owned, non-detached
  worker: return an immediately durable `in_progress` resource, allow polling
  and cancellation, reject active deletion with 409, and cancel/join all work
  through a route shutdown hook before backend teardown. Queue termination
  also releases workers waiting for llama-server to leave its idle sleep state.
  Background `store: false` uses the same SQLite resource backing as
  `store: true` because polling requires server-side state. The existing
  llama-server task/slot queue remains the only scheduler.
- [x] Adopt fail-stop restart semantics: atomically mark orphaned active
  snapshots `failed` with `server_restarted` during route startup, without
  attempting to recover model execution or affecting fresh requests.
- [x] Parse and validate repeated retrieve `include[]` parameters. Seven
  documented projections are accepted as materialized values or truthful
  no-ops: `code_interpreter_call.outputs`,
  `computer_call_output.output.image_url`, `file_search_call.results`,
  `message.input_image.image_url`, `message.output_text.logprobs`,
  `web_search_call.action.sources`, and `web_search_call.results`. Persisted
  `reasoning.encrypted_content` is rejected because no encrypted payload is
  stored.
- [x] Normalize `parallel_tool_calls: null` to the documented `true` default in
  both inference and the response echo. Accept omitted/null `moderation` and
  continue rejecting non-null moderation because there is no provider or
  documented disabled object to honor.
- [x] Capture and sanitize the complete 2026-08-25 standalone Codex 0.149.1
  create request and its six tool declarations as a deterministic fixture, and
  classify every observed top-level field in focused tests.

Completed spine coverage:

- [x] Complete bounded differential/live coverage for late generation errors,
  disconnect cancellation, and genuinely generated interleaved tool calls.
  Deterministic tests cover sync/SSE projection, late failure envelopes, parser
  correction, limits, typed persistence failures, cancellation polling,
  namespace tools, and interleaved function/custom/local-shell calls. A live
  disconnected stream emitted an actual server task cancellation and released
  the only slot; a live Qwen stream then generated two distinct parallel calls
  across 88 ordered Responses events.

HTTP/resource conformance ledger, ordered by Codex-session benefit relative to
implementation cost:

#### Tier 1: completed checkpoint

- [x] Add foreground `POST /v1/responses/{response_id}/cancel` through the
  active sink registry, with active, completed, missing, repeated, and
  cancel-versus-complete outcomes. This is intentionally a local interactive
  extension to OpenAI's documented background-only cancel operation.
- [x] Capture the create request emitted by standalone Codex 0.149.1 on
  2026-08-25 as a deterministic sanitized fixture. Classify each observed
  field as implemented, a truthful metadata/no-op field, or explicit
  unsupported behavior instead of auditing an unbounded schema.

#### Tier 2: completed cheap compatibility

- [x] Parse repeated retrieve `include[]` values and validate all eight
  documented names observed in the current reference: seven are truthful
  materialized/no-op projections, while persisted
  `reasoning.encrypted_content` is explicitly rejected. Streamed retrieval and
  resume remain separate backlog items.
- [x] Accept omitted/null `moderation` and reject every non-null value. The
  current public contract does not document a disabled moderation object, so
  inventing one would not be a truthful compatibility feature.
- [x] Normalize and echo `parallel_tool_calls: null` as `true`, matching the
  request default and the generation input consumed by llama-server.

The foreground replay pipeline is not an unquantified “remaining typed
resources” bucket. It currently recognizes 16 named replay/input item shapes:
`message`, three client call shapes, seven client/hosted output shapes,
`reasoning`, two compaction shapes, `ghost_snapshot`, and `item_reference`. It
also recognizes nine named content shapes: `input_text`, `output_text`, `text`,
`reasoning_text`, `refusal`, `input_image`, `computer_screenshot`, `input_file`,
and MCP `image`. The known gaps are split explicitly below:

- [ ] Add the four newer client-executed replay item families—`shell_call`,
  `shell_call_output`, `apply_patch_call`, and `apply_patch_call_output`—when
  Codex negotiates them. The current catalog deliberately negotiates the
  already-supported local-shell and custom-tool forms.
- [ ] Add direct `input_audio` content if a deployed llama-server adapter can
  consume it. Audio and video carried by `input_file` are already lowered.
- [ ] Resolve provider-backed `file_id` references for file/image content in
  Phase 4. Inline/data content is already supported; inventing a local file
  service is not Phase 3 work.
- [ ] Add model-produced audio/image and hosted-tool-specific output variants
  only with the corresponding truthful model/provider capability.

#### Evidence-triggered compatibility, not a parity project

- [ ] Add any additional request-echo field which an SDK reader requires but
  the native response snapshot does not yet preserve. Do not copy fields merely
  because they exist in the create schema.
- [ ] Correct foreground status, incomplete details, usage, and event/error
  ordering when an SDK or Codex fixture exposes an observable mismatch. Do not
  pursue byte-for-byte streams or exact production timing as independent
  goals.
- [ ] Add concurrent retrieve semantics for an active foreground response only
  if a real client uses them; the active registry introduced for cancellation
  makes this inexpensive. Background polling already retrieves its durable
  active snapshot.
- [ ] Add streamed retrieval with `starting_after` only when resumable event
  delivery becomes a supported profile.
- [ ] Add WebSocket transport independently of streamed HTTP retrieval; it is
  Phase 5 work, not part of making the foreground SSE profile honest.

Completed supporting work:

- [x] Connect native active and terminal snapshots to store compare-and-swap,
  including safe reconciliation with lineage detachment during generation.
- [x] Add an aggregate inline-media request budget without changing the
  resource API.
- [x] Validate `user`, `safety_identifier`, and `prompt_cache_key` as
  non-inference identifiers; lower `parallel_tool_calls` to generation; and
  normalize its nullable default consistently in the response echo.

Deferred operational/stateful work:

- [ ] Persist a canonical event journal only when streamed background create or
  resumable retrieval is selected. The journal would retain emitted event
  sequence numbers and deltas so a reconnecting client can resume by cursor;
  ordinary snapshot polling and fail-stop restart behavior do not need it.
- [ ] Add expiry and garbage collection when retention becomes operationally
  necessary.
- [ ] Add expired-resource fixtures with the expiry feature, not as a Phase 3
  foreground prerequisite.
- [ ] Add durable byte accounting and external blob policy if response media is
  moved out of canonical SQLite snapshots.
- [ ] Define an operational retention policy only for a deployment which needs
  one; OpenAI's policy is not automatically this server's policy.

Gate:

- [x] Native generation is the default for the ordinary advertised foreground
  profile.
- [x] The installed sidecar path constructs no Responses events in generic task
  code. Upstream's own limited renderer remains compiled and unchanged for the
  explicit no-extension configuration.
- [ ] The Phase 3 conformance matrix is green for all advertised HTTP
  capabilities.
- [ ] Official SDK objects round-trip without local patches across the complete
  advertised matrix.
- [x] Stored continuation and deletion tests cover text, reasoning, tool calls,
  and multimodal tool outputs.
- [ ] Foreground error fixtures cover malformed, unsupported, missing,
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
- [x] Capture standalone Codex 0.149.1 model discovery and its first complete
  create/tool-negotiation request on 2026-08-25 as a sanitized, reproducible
  black-box fixture independent of a live model.

Outstanding work:

- [ ] Replace the Qwen-alias special case with configurable or model-derived
  capability policy for arbitrary local and router models.
- [ ] Add end-to-end fixtures for API prefixes, sleeping/cached models, and
  actual router-process handler substitution; router-shaped catalog projection
  already has focused unit coverage.
- [ ] Advertise reasoning summaries, verbosity, hosted search, and other Codex
  capabilities only as their server implementations become truthful.
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

### Phase 5: additional resources and alternate transports 🕒

Purpose: add the remaining stateful resources and transports when a concrete
client needs them. High-availability job recovery and llama-server router mode
are deployment profiles, not implicit requirements of a correct single-server
Responses endpoint.

Completed foundations:

- [x] Define typed response statuses and enforce basic terminal transition and
  revision invariants in the store.
- [x] In Phase 3, implement an owned, non-detached background worker with an
  immediately durable `in_progress` resource, polling, cancellation,
  active-delete 409, and graceful cancel/join before task-queue teardown.
- [x] Use llama-server's existing task/slot queue as the only admission
  scheduler. An accepted background response is immediately `in_progress`;
  that status can include time waiting for a model slot. Under the trusted
  single-user profile, a second queue would duplicate scheduling without an
  additional admission or backpressure requirement.
- [x] Use the same SQLite resource backing for background `store: false` and
  `store: true`. Polling an asynchronous response inherently requires
  server-side state, and OpenAI documents temporary disk persistence for
  background `store: false`. The echoed flag and retention promise remain
  distinct from `store: true`; a second ephemeral implementation is YAGNI.
  This local profile does not yet implement timed deletion, so callers that
  require physical removal must use the delete route until expiry/GC is
  selected.
- [x] Adopt explicit fail-stop restart behavior instead of inference recovery.
  Graceful shutdown cancels and joins active jobs. After a violent exit, the
  next route startup marks orphaned `queued`/`in_progress` snapshots `failed`
  with `server_restarted`; it never replays or resumes model generation.
- [x] Keep the core honest about its remaining bounds: background streaming,
  event replay, garbage collection, conversations, compact, WebSocket, and
  router-mode stateful resources are not advertised.

Outstanding work:

- [ ] Implement compact using the same response/item store and context policy.
- [ ] Implement conversation attachment and any companion conversation
  resources required for a client to use the create contract normally.
- [ ] If a supported client needs `background: true, stream: true`, add a
  canonical event journal—an append-only sequence of emitted Responses
  events—then use it for background streaming, reconnect by cursor, and
  `starting_after` retrieval. Snapshot persistence alone cannot reproduce
  already-emitted deltas after a connection is lost.
- [ ] Implement WebSocket transport if a supported client needs it.

Conditional deployment profiles, not current work:

- **Front-door admission and backpressure.** A public or high-concurrency
  deployment may need a bounded executor which exposes `queued` while a
  background request waits, or rejects excess work. The trusted single-user
  profile does not need a second queue in front of llama-server's task/slot
  scheduler; add one only when thread/job-count limits become an operational
  requirement.
- **Router-mode Responses resources.** Router mode is llama-server's
  no-model front process which dynamically starts model-serving child
  processes. Today it proxies create and token-count requests, while
  retrieve/cancel/delete are not installed there. Supporting those resource
  routes would require the router to record `response_id -> child/model`
  affinity and send every follow-up to the creator. Persist that map only if
  the router itself promises resource continuity across its own restart.
- **High-availability inference recovery.** Resuming an interrupted token
  stream would require durable job inputs, sampler/model state, ownership
  leases, and an event journal. The single-server profile explicitly terminates
  interrupted resources instead; new sessions and new requests remain usable.

Deferred backlog:

- [ ] Add expiry and garbage collection only when a retention policy is chosen.
- [ ] Add durable blob accounting/externalization only if SQLite snapshots stop
  carrying response media directly.

Gate:

- [x] The advertised non-streaming background core passes SDK-level
  create/poll/terminal/delete coverage and deterministic active cancel,
  repeated cancel, active-delete, and shutdown tests.
- [x] Restart reconciliation has deterministic SQLite coverage and is
  idempotent; already-terminal resources are unchanged.
- [ ] State-machine race tests cover disconnect/reconnect or router child loss
  only when those future profiles are advertised.
- [ ] If WebSocket or resumable retrieval is advertised, every transport is a
  projection of one canonical event history.
- [ ] Compact passes SDK-level tests.
- [ ] If router-mode stateful resources are advertised, create/continue/
  retrieve/cancel/delete pass end to end through the owning child.

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
- [x] Restore upstream's stock Responses implementation as the no-extension
  default while making the installed sidecar path independent of it. This
  avoids carrying a fork-legacy renderer without deleting upstream behavior.
- [x] Run targeted dated probes against the real OpenAI endpoint for defaults,
  validation errors, max-output lifecycle, structured output, tool policy, and
  continuation lineage; commit the sanitized `/input_items` lineage fixture.

Outstanding work:

- [ ] Add a dated, targeted OpenAI probe only when a suspected observable
  schema, validation, error, or event-order mismatch cannot be resolved from
  the public contract. Commit a sanitized semantic fixture; byte-for-byte
  differential testing and production timing equivalence are not goals, and
  normal development must not require credentials.
- [ ] Expand Codex black-box suites with scripted generation and live Qwen smoke
  tests beyond the single canonical turn.
- [ ] Fuzz request parsing, SSE framing, replayed items, provider output, and
  cancellation sequences.
- [ ] Soak parallel responses, storage eviction, disconnects, router churn, and
  hosted-tool loops.
- [ ] Audit authorization boundaries, SSRF/path traversal, subprocess handling,
  secret redaction, size limits, and denial-of-service surfaces.
- [ ] Document supported, experimental, and unavailable capabilities and
  automate advancement of the OpenAI spec snapshot.

Gate:

- [ ] All advertised conformance rows pass across sync, SSE, and applicable
  WebSocket modes.
- [ ] Codex completes a broad tool/media/state test corpus against Qwen.
- [x] The installed sidecar has no dependency on generic Responses JSON/event
  logic; the unchanged upstream stock path remains available only when the
  extension is not installed.
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
    --ctx-size 180224 \
    --reasoning auto \
    --reasoning-effort low \
    --chat-template-kwargs '{"preserve_thinking":false}' \
    --temperature 1.0 \
    --top-p 0.95 \
    --top-k 20 \
    --min-p 0.0 \
    --presence-penalty 0.0 \
    --repeat-penalty 1.0 \
    -np 1 \
    -ngl -1 \
    -ctk q8_0 \
    -ctv q8_0 \
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

Verified live on 2026-08-24 with the `codex` resolved from `PATH` and the then
90,112-token BF16-KV Qwen profile. That process resolved
`/home/ken/.local/bin/codex`, which
reported `codex-cli 0.149.1`; separate desktop/editor helper binaries were also
installed but were not the smoke process. An unauthenticated catalog request
returned 401; and the authenticated catalog
returned the Qwen alias, loaded context, text/image modalities,
`low`/`medium`/`xhigh`, low default, and the shared base instructions. The first
cold pinned-model process warned that it selected unknown-model fallback while
also refreshing the catalog, yet completed exactly `LOCAL_RESPONSES_OK` through
`/v1/responses` (86,729 input tokens, including nine connector namespaces with
211 nested functions, and 39 output tokens). The second identical process used
the refreshed catalog with no warning, reused 86,725 cached prompt tokens, and
again completed exactly. This is an end-to-end compatibility observation, not
a performance or output-determinism contract.

Reverified on 2026-08-25 after making native generation the production
foreground path. An authenticated synchronous create returned exactly
`NATIVE SPINE OK`; retrieve returned the same typed reasoning/message snapshot
and usage from SQLite; and the installed Codex CLI completed exactly
`NATIVE_CODEX_OK`. A second black-box turn generated a local-shell call, Codex
executed `/bin/bash -lc pwd` in the requested repository, replayed the tool
result, and eventually completed exactly `NATIVE_TOOL_ROUNDTRIP_OK`.

Reverified again after the Cortés restore-to-upstream checkpoint. The live
typed path passed authenticated catalog projection, sync create/retrieve,
continuation-aware input counting, structured 404s, explicit unsupported
telemetry and file-provider errors, a 48-event forced function-call stream,
the two-image rich tool result, disconnect cancellation, and an 88-event
model-generated parallel two-call stream with distinct `fc_*`/`call_*` IDs.
The first installed-Codex attempt exposed an instructions-plus-developer
canonicalization bug; a focused lowering regression now protects it. A fresh
Codex process then negotiated the remote catalog and completed exactly
`CORTES_CODEX_OK` with 86,660 input and 54 output tokens.

That tool turn also exposed a deployment-pressure warning worth retaining: the
full Codex prompt/tool catalog begins around 86.7K tokens inside this 90,112
token slot. Codex compacted twice and Qwen unnecessarily repeated `pwd` before
the final answer. The protocol round trip passed, but this is not yet a green
tool-use determinism or context-budget result. That observation motivated the
current 180,224-token Q8-KV live-test profile: preserve the truthful tool
catalog, trade cache precision instead of protocol coverage, and leave the
production-quality model profile as a separate deployment choice.

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

At the 2026-08-25 Cortés checkpoint, the diff against current upstream changes
about 370 lines across six existing `tools/server` files. The generation
contract, adapters, route extensions, conformance tests, and all protocol/state
code are additive files. Every other upstream server source was restored
byte-for-byte; the six exceptions contain the narrow declared seams and the
separately documented PR #27196 MTP logprobs fix. This is the merge-cost metric
to preserve; raw additive line count is not the concern.

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

- Which fields newly observed in a Codex or SDK request are meaningful for
  local inference, truthful metadata/no-ops, or explicit unsupported behavior?
- If resumable retrieval or background recovery is selected, what is the
  smallest event-journal schema which supports it without replacing the
  versioned SQLite snapshot store?
- What real client requires WebSocket/resume behavior, and which observable
  subset does that client consume?
- Where should model-specific opaque reasoning state live so that replay is
  correct but protocol code remains model-neutral?
- Which hosted tools require explicit user approval, and how is that policy
  configured without putting deployment concerns in the request parser?

Each spike should end in a small test or an ADR amendment in this document.

## Next conformance slice

The Cortés architecture checkpoint is complete. Ordinary foreground creation
and input-token counting now share typed lowering; the sidecar owns Responses
objects, events, and persistence; and the installed route has no legacy
renderer, hidden forwarded request, or fallback selector. The bounded
generation-spine evidence is now complete as well. Active cancellation, the
dated Codex request fixture, retrieve projections, and the non-streaming
background core are also implemented.

Recommended next checkpoint:

1. Turn the already-observed OpenAI cancellation outcomes—idempotent repeated
   cancel, completed-background and synchronous-foreground rejection, and
   missing-resource 404—into one small dated semantic fixture. Probe only the
   background create/poll and terminal-race details still missing. Keep the
   local foreground-cancel extension separate from official background-only
   semantics.
2. Exercise authenticated real-generation cancellation and background polling
   with Qwen, including a clean server shutdown with active work. Deterministic
   ownership/race tests already protect the implementation; this slice is
   specifically differential and live evidence.
3. Add SDK-decoded end-to-end coverage that combines continuation and
   `item_reference` replay with rich multimodal function/custom tool results.
   The lower-level storage and lowering tests already pass independently.
4. Exercise an authenticated Codex/Qwen browser-extension-shaped round
   trip. This is the highest-value live stress test for the feature that
   motivated multimedia Responses support.
5. Add additional request echoes or the four quantified replay-item families
   only if the fixture or live Codex run reaches them.

Long-tail features remain in their owning phases: neutral progress/per-token
timing events if those llama extensions are ever advertised, provider-backed
`file_id` resolution, externalized durable media blobs and retention policy,
hosted providers, event journals, streamed background/resume, WebSocket
transport, expiry/GC, high-availability inference recovery, and router-mode
stateful resources. Their absence must remain explicit, but none is a reason to
reintroduce a second Responses renderer.
