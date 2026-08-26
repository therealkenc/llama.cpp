# Llama Responses architecture and operating notes

Stable design decisions, compatibility behavior, deployment commands, and
maintenance policy live here. Active work belongs in [`sprint.md`](sprint.md);
landed checkpoints and evidence belong in [`completed.md`](completed.md).

Spec snapshot: 2026-08-26

## Decision

We use dependency inversion to put a statically linked Responses
subsystem inside the existing `llama-server` executable.

The subsystem lives under `tools/llama-responses/` and builds as a library,
not as another executable. The existing `llama-server` remains responsible for
command-line parsing, process lifecycle, authentication, HTTP, model loading,
router mode, slots, metrics, and all non-Responses endpoints. The new subsystem
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
- SSE event types, ordering, indices, and sequence numbers, plus WebSocket
  behavior only if that optional transport is advertised;
- continuation, storage, retrieval, deletion, cancellation, compaction, and
  background behavior;
- function/custom tool round trips and server-hosted tool execution;
- error status, shape, code, parameter attribution, and cancellation behavior.

This does not mean producing identical model tokens. It means implementing the
same observable protocol and lifecycle for each capability we advertise.
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
  Once installed, the sidecar owns all Responses routes and must return an
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
it makes each new Responses feature cut across several concerns:

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
- `GET /v1/responses/{response_id}`, as either a snapshot or resumed
  background SSE stream;
- `DELETE /v1/responses/{response_id}`;
- `POST /v1/responses/{response_id}/cancel`;
- `POST /v1/responses/compact`;
- `GET /v1/responses/{response_id}/input_items`;
- a WebSocket entry point if that optional profile is later selected.

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
The `response_store` interface has an in-memory test implementation with a
configurable visible-resource limit and a production SQLite implementation.
The interface owns atomic response nodes, original direct input items,
generated output items, status transitions, metadata, pagination,
compare-and-swap revisions, lineage materialization, tombstones, and the
canonical background-stream event journal. Its generation write is deliberately
narrower than generic resource replacement: an active checkpoint advances a
small scalar head and appends an event batch, while a terminal checkpoint
installs the complete output snapshot and item index once. In-process active
generation ownership lives in the route-lifetime registry rather than the
durable store. Expiry, physical reclamation, and response-to-router recovery
remain extensions of that contract.

The Phase 3 journal deliberately uses one SQLite row per logical Responses
event. A background head update and its event batch commit in one transaction,
and no event is released to an HTTP subscriber before that commit succeeds.
This is the replay contract; segmented blobs would be a future physical
optimization, not a different API semantic.

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
- immutable parent-linked lineage and tombstone-aware context materialization;
- deterministic visible-resource limits in the in-memory test store.

Production resources persist in `~/.cache/llama.cpp/responses.sqlite3`;
`LLAMA_RESPONSES_DB` can override the complete path. Schema v5 physically
separates the frequently updated scalar head from cold direct-node and terminal
snapshots. A node stores its optional `previous_response_id`, this request's
direct normalized input, and this response's output; it does not copy its
ancestors' prompt. New serialized payload-v2 writes likewise omit the redundant
fully expanded input. Item indices and background SSE events remain
transactional members of the resource. This layout is a correctness-relevant
performance invariant: neither prompt size, the generated prefix, nor ancestor
lineage may be rewritten for each token or copied into each descendant. A
background request is immediately checkpointed as `in_progress` and uses
SQLite even when its response body truthfully echoes `store: false`, because
polling requires a resource after the create request returns. This is not a
contract mismatch:
OpenAI likewise temporarily persists background `store: false` responses so
they can be polled. `store` controls the API's retention promise, not whether an
asynchronous implementation may write temporary bytes. This local deployment
therefore uses one SQLite backing for both values and deliberately has no
separate ephemeral store.

The optional parent edges form a response forest, or equivalently the
single-parent subset of an immutable DAG, with arbitrary fan-out. At read time,
`/input_items` walks the retained chain oldest-to-newest and returns ancestor
input/output contributions followed by the target's direct input. Continuation
uses that same view plus the target's output. Thus inference remains
proportional to the context actually sent to the model while durable storage is
proportional to the new turns. Corrupt cycles, missing non-legacy ancestors,
malformed checkpoints, and duplicate item IDs are store errors rather than
silent truncation.

Delete is a soft visibility transition. Public retrieve, `/input_items`, direct
`item_reference`, and selection as a new `previous_response_id` treat a
tombstoned response as absent. Internal materialization may still cross that
node for descendants created while it was visible, so deleting an ancestor does
not rewrite or revise a child. Output item IDs remain globally reserved while
their owning tombstone exists, but public item lookup does not expose them. The
v5 migration retains a pre-v5 `detached_context` only as a read-only legacy
lineage checkpoint for a database whose old hard-delete policy already removed
an ancestor. The v4 `continuation_input_items` field supplies that node's direct
request contribution, so its redundant expanded `input_items` can be ignored.
New writes and new deletes never create legacy checkpoints. Physical
reclamation is deferred to a retention/GC phase.

The in-memory store's configured limit counts publicly visible resources. It
may tombstone the oldest visible terminal node, but retains any node needed for
lineage, so this is deliberately not a hard byte bound. If all visible
resources are active, admission fails deterministically rather than deleting
live state.

The schema does not persist an expiry policy or router ownership. It also does
not attempt to resume inference after process death.
Graceful shutdown cancels and joins owned jobs; on the next startup any durable
`queued` or `in_progress` snapshots left by a violent exit are atomically
changed to `failed` with error code `server_restarted`. A journaled response
receives the corresponding terminal `response.failed` event at its next
sequence number. Earlier journal deltas remain replayable, but the failed
snapshot does not resurrect or resume partial inference. Fresh requests are
independent of those terminal records.
These policies must not change the route or orchestration API if a richer
deployment profile is added later.

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

## Codex CLI model-catalog compatibility

This is a Codex client contract, not part of the public OpenAI Models or
Responses APIs. Codex uses model metadata to choose instructions, tools,
context limits, modalities, and request options. Merely accepting Responses
requests is enough for a basic turn, but it is not enough for deterministic
Codex behavior.

The conformance claim in this section is intentionally one model and one
endpoint: `qwen3.8-27b-local` on the canonical deployment. The route seam can
already inspect other downstream catalog shapes, but heterogeneous capability
policy, multi-model dispatch, and router resource affinity belong to Phase 7.

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
| instructions | One inspectable Apache-2.0 [base-instructions file](../prompts/codex-base-instructions.md), initially imported from OpenAI Codex. It is a locally owned deployment baseline, not selected by `client_version` and not a prompt registry. Model-tuned changes remain future, eval-driven policy. |

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
evidence that all hosted OpenAI model catalog entries use identical
instructions. Its source commit, checksum, license, and NOTICE are recorded in
the adjacent [prompt README](../prompts/README.md).

The fallback is useful but not capability-neutral. With command auth enabling
refresh and the decorator active, the canonical Qwen alias resolves from the
remote catalog after refresh. A cold pinned-model turn may still warn before
the refreshed cache is selected. A future Codex client that adds an actually
required field may fall back until the projection is extended; compatible JSON
additions require no server change and never justify an exact-version route.

### Codex namespace loading, tool search, and Responses Lite

Installed Codex apps/plugins contribute `namespace` tool containers. They are
client tools, not OpenAI-hosted tools: Codex searches its local catalog and
executes the selected function/custom call. The endpoint owns the typed
Responses protocol around that client work.

The canonical Qwen catalog advertises two independent capabilities:

- `supports_search_tool: true` selects client-executed deferred discovery;
- `use_responses_lite: true` selects Codex's compact internal request
  representation.

Search is the semantic capability. Lite is an additive wire optimization, not
an alias for search. A model entry may truthfully enable one without the other.
OpenAI's [tool-search guide](https://developers.openai.com/api/docs/guides/tools-tool-search)
documents the discovery protocol and the positional `additional_tools` item;
[Codex issue #31882](https://github.com/openai/codex/issues/31882) is useful
evidence that current GPT-5.6 Sol/Terra/Luna catalog entries also select the
Lite transport.

For client search, the model emits a `tool_search_call` with
`execution: "client"`, an object-valued `arguments`, and a non-null `call_id`.
Codex returns a correlated `tool_search_output` containing the selected tool
definitions, then executes any later namespaced call itself. Both items are
ordinary typed lineage: they survive SQLite storage, retrieval, restart,
`previous_response_id`, and complete manual replay. Selected definitions join
the active response `tools` projection and the next model request. Hosted
`execution: "server"` search remains a Phase 4 provider concern.

Responses Lite sends
`X-OpenAI-Internal-Codex-Responses-Lite: true`, omits top-level `instructions`
and `tools`, and begins `input` with a developer-role `additional_tools` item
followed by a developer message containing the base instructions. Flat core
tools are grouped in a `functions` namespace. Codex also sends
`parallel_tool_calls: false` and `reasoning.context: "all_turns"`. The sidecar
preserves `additional_tools` with an `at_` input-item ID, projects its
declarations through the same tool validator/lowerer, and treats `all_turns`
as truthful because generation already materializes the complete response
lineage. `additional_tools` is input context and emits no output-item event.

Exact duplicate declarations collapse; conflicting definitions fail before
generation. Current Codex prepends one complete `additional_tools` snapshot on
each HTTP full replay. The endpoint accepts repeated identical snapshots and
merges compatible selected namespace members. It does not claim a historical
time-travel policy for exotic hand-authored requests which place conflicting
snapshots at several positions; those are rejected rather than guessed at.

The shared llama chat-template renderer still places the currently callable
schemas in its ordinary tool prefix. Selecting a tool can therefore invalidate
that prefix cache once instead of literally splicing the new schema at the end
of an already rendered prompt. This is a conscious reuse of the server seam,
not a second renderer. The much smaller prefix then remains stable: the live
Chrome run's final 29,650-token request reused 29,546 cached tokens. An
end-of-context insertion hook is justified only if a measured deployment makes
that one discovery transition material.

Clients which omit search retain eager namespace lowering. Qwen chat templates
only accept a flat tool list and cap function names at 64 characters, so that
path validates each namespace, maps `{namespace, name}` to a collision-safe
model name, and reverses the mapping on generated calls.

The size change is the practical reason both catalog flags are enabled. The
same installed-plugin setup produced these 2026-08-26 measurements:

| Signal | Eager fallback | Search + Lite |
| --- | ---: | ---: |
| Serialized first request | 293,496 characters | 32,390 bytes in the isolated Lite capture |
| Tool declarations | 262,896 characters / 58,057 Qwen tokens | one compact core namespace plus client search |
| Initial Qwen prompt | 88,439 tokens | 11,688 tokens in the Chrome run |
| Largest Chrome continuation | context workaround required | 29,650 input tokens, 29,546 cached |

The authenticated Chrome run used the 98,304-token MTP profile, made one
client search call, loaded the Chrome bridge, executed eleven client MCP calls,
and returned the selected tab without prompt truncation. Its thirteen model
rounds used 299,202 cumulative input tokens because each continuation reports
the full prompt again; peak prompt occupancy was only 29,650 tokens. A separate
exact-`OK` turn used 11,648 input and 21 output tokens, with 4.139 seconds of
prompt evaluation and 217 milliseconds of generation. The model's wandering
tool strategy is not protocol overhead and does not reintroduce the old 88K
first-turn catalog.

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
9. Multi-model/router tests: heterogeneous catalogs, model selection, and
   stateful follow-up affinity across actual child processes.
10. Optional real-OpenAI differential runs: protocol evidence, never a required
   unit-test dependency.

Reproduce bugs at the lowest layer that can express them. Golden
fixtures must assert the whole event stream, not just concatenated text.

### Canonical live Qwen server

The live Codex/conformance profile runs on port 8081. This is intentionally
separate from Docker-hosted services on port 8080 and from the high ports used
by automated server tests. It uses the
[Qwen3.8-27B model](https://huggingface.co/Qwen/Qwen3.8-27B).

```bash
MODELPATH="$HOME/Devel/scripts-ken/gguf/unsloth/Qwen3.8-27B-GGUF"
MODEL="Qwen3.8-27B-UD-Q4_K_XL.gguf"
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
    --ctx-size 196608 \
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
    -ngl 99 \
    -ctk q8_0 \
    -ctv q8_0 \
    --kv-unified \
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

Historical live Qwen and Codex verification transcripts are retained in
[`completed.md`](completed.md#historical-live-verification).

## Upstream merge discipline

The fork earns its keep only if Responses can evolve without turning each
upstream merge into archaeology.

- New protocol, state, provider, and conformance code stays under
  `tools/llama-responses/`.
- Generic server code exposes neutral capabilities; it does not import
  Responses item/event vocabulary.
- Prefer one explicit route-construction hook over edits beside individual
  routes.
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
code are additive files. The other upstream server sources were restored
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
