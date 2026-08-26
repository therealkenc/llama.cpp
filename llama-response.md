# A first-class OpenAI Responses API in `llama-server`

Status: core OpenAI Responses resource complete through Phase 3; the
single-model Codex catalog MVP is complete. Hosted-tool execution is next.
Spec snapshot: 2026-08-25

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

## Current compatibility baseline

The sidecar provides or protects these valuable behaviors, with fixture gaps
called out below:

- string and item-array input, instructions, developer/system folding, and
  multi-turn assistant replay;
- text, replayed refusal content, reasoning, function/custom/local-shell/tool-search/web/file/
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
  background create, durable polling, disconnect-resumable SSE, cancellation,
  soft deletion of terminal resources, and an explicit conflict while deletion
  races active work.

Remaining areas to correct or complete include:

- request fields and response projections newly exposed by future Codex or SDK
  fixtures, classified as implemented, truthful no-ops, or explicit
  unsupported behavior;
- provider-backed `file_id` resolution rather than recovery placeholders;
- the four newer client-executed replay item families not typed by the current
  lowerer: `shell_call`, `shell_call_output`, `apply_patch_call`, and
  `apply_patch_call_output`;
- compact and conversations; WebSocket transport and router-mode stateful
  resources remain optional later profiles;
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
process restart. Schema v5 stores a scalar-only hot head, cold immutable
direct-input/output nodes linked by `previous_response_id`, nullable deletion
tombstones, globally reserved output-item IDs, and the canonical
sequence-numbered event journal used by background SSE. New payload-v2 rows do
not duplicate direct request input inside a materialized conversation snapshot.
The migration reader treats an old v4 `detached_context` as a single read-only
legacy checkpoint and never creates a new one.

`/input_items` and continuation materialize the retained response graph on
read, crossing tombstoned ancestors only for already-existing descendants.
Deleting an interior ancestor, or deleting the parent after generation starts
but before the child is committed, therefore cannot strand that child's future
lineage and does not rewrite or increment any descendant.
For background `stream: true`, each complete SSE batch is released only after
its corresponding head-and-journal checkpoint commits. Foreground SSE has no
resume contract: its active projection remains in process and `store: true`
installs the complete durable snapshot at the terminal boundary instead of
rewriting it for each token. No terminal is exposed before the terminal CAS
succeeds. A storage failure
emits an SDK-decodable, transport-level `response_store_error`, requests
generation cancellation, and best-effort hides the unusable partial
resource. It is not represented as a durable `response.failed` outcome because
the persistence layer needed to record that outcome has failed.

The opt-in `bench-llama-responses-generation` target guards the hot-path
architecture without turning host timing into a CTest gate. It drives the real
generation sink and canonical journal with synthetic chunks:

```bash
build/bin/bench-llama-responses-generation
```

On the development host, the pre-refactor one-run baseline and the post-refactor
median of three were:

| Workload | Before | After |
| --- | ---: | ---: |
| SQLite journal, 4,000 × 64-byte deltas | 6.43 s | 186 ms |
| SQLite journal, 2,000 × 4-byte deltas with 200 KiB static input | 16.73 s | 94 ms |
| No store, 4,000 × 64-byte deltas | 30.5 ms | 4.65 ms |
| In-memory journal, 4,000 × 64-byte deltas | 110.8 ms | 11.9 ms |

The schema-v5 graph refactor was then checked with five-run medians against the
immediately preceding checkpoint:

| Workload | Previous checkpoint | Schema v5 |
| --- | ---: | ---: |
| SQLite journal, 1,000 × 64-byte deltas | 47.896 µs/chunk | 50.418 µs/chunk |
| SQLite journal, 2,000 × 64-byte deltas | 47.240 µs/chunk | 47.755 µs/chunk |
| SQLite journal, 4,000 × 64-byte deltas | 46.480 µs/chunk | 49.070 µs/chunk |
| SQLite journal, 200 KiB static input + 2,000 × 4-byte deltas | 47.380 µs/chunk | 47.141 µs/chunk |
| SQLite journal, 200 KiB tombstoned ancestry + 2,000 × 4-byte deltas | 45.065 µs/chunk | 46.535 µs/chunk |

The same run measured deep fixed-size response chains separately from the hot
journal path:

| Nodes | SQLite materialization | In-memory materialization | SQLite bytes | Materialized view bytes |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 1.945 ms | 0.210 ms | 135,168 | 46,893 |
| 256 | 8.101 ms | 0.824 ms | 458,752 | 187,941 |
| 1,024 | 39.597 ms | 4.262 ms | 1,671,168 | 752,469 |

The database and resulting view grow approximately linearly with the number of
fixed-size turns rather than storing each materialized prefix. Prompt
materialization is now an explicit read/create cost. Active text and tool
values use append-only typed accumulators, and committed events are not retained
a second time in the projection. The one-row-per-logical-event SQLite journal
remains near its prior performance class at approximately 47–50 microseconds
per committed chunk; the modest spread
above is normal host variance rather than a prompt- or lineage-size multiplier.

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
state machine owns the actual sync body and SSE bytes. Initial and terminal
snapshots are written directly to SQLite with compare-and-swap; active
generation advances an independent revision on the small response head.
Ancestor deletion only tombstones that ancestor and does not compete with the
child's generation marker. The child keeps its immutable parent edge, and its
canonical context is materialized from direct nodes when required.
An active-response registry binds each live response ID to that same neutral
sink. The cancel route requests generation cancellation, waits for the sink's
terminal acknowledgement, and returns the canonical cancelled snapshot;
missing, repeated-cancel, already-terminal, and cancel-versus-complete outcomes
have deterministic coverage. OpenAI documents this route only for
`background: true`; accepting it for an active foreground response is a tested
local extension, not a claim about the official foreground contract.

`background: true` uses an owned, joinable worker and an owned copy of the
server request. Create first durably records an `in_progress` resource;
non-streaming create returns that snapshot for polling. With `stream: true`,
the worker privately drains llama-server's model stream into atomic
snapshot-plus-event transactions per accepted batch while the public create
connection reads the SQLite journal. Subscriber disconnect therefore releases
only that reader and does not cancel generation. A later
`GET /v1/responses/{id}?stream=true&starting_after=N` replays events after the
cursor and, while persistence remains available, tails the same journal to its
terminal event. A persistence failure instead ends the transport with the
fail-stop error described above. Both `store` values use this resource backing
because background retrieval requires server-side state.

The documented
[`starting_after`](https://developers.openai.com/api/reference/cli/resources/responses/methods/retrieve)
parameter is an exclusive sequence cursor, not an assertion that the named
event already exists. A 2026-08-25 OpenAI probe returned HTTP 200 with an
empty SSE body for a terminal response when the cursor equaled or exceeded the
tail, including a cursor of 536 for a ten-event response. The same probe kept an
active future-cursor retrieval pending. The sidecar follows that behavior:
future events remain skipped until terminal, then the stream ends empty. It
still rejects malformed or overflowing cursors and detects a missing row inside
an otherwise committed suffix as journal corruption.

Cancel reaches the same active sink, delete returns 409 while work is active,
and terminal deletion tombstones the public resource while retaining its
journal and lineage for existing descendants. The route bundle's shutdown
hook cancels and joins workers before llama-server tears down its task queue;
the generation seam also treats queue-first termination as cancellation rather
than asserting on shutdown ordering. llama-server's existing task/slot queue is
the admission scheduler, so the Responses layer does not duplicate it merely
to expose a momentary `queued` status. There is deliberately no GC,
router-mode resource ownership, or inference resurrection in this core.
Startup instead terminalizes active snapshots orphaned by a prior process death
and appends the matching `response.failed` journal event.

The installed route bundle receives only this typed generation service and
therefore cannot fall back to stock or fork-legacy Responses rendering. The
upstream stock route remains available only when no sidecar extension is
installed. The llama telemetry extensions `return_progress: true` and
`timings_per_token: true` are currently rejected with parameter-attributed
unsupported errors; `false` is accepted as a no-op. Supporting them later
requires neutral typed updates, not a hidden second renderer.

The typed lowerer preserves `call_id` independently from public output-item IDs
and carries mixed text/image/file tool results in prompt order. It resolves
materialized continuation views and item references, accepts data-URI and
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
standalone Codex 0.149.1, with dynamic fields normalized and the observed
top-level fields classified by focused tests.

The advertised profiles deliberately fail closed for long-tail or stateful
features which are not implemented yet:

- conversation resources and compact;
- WebSocket transport;
- expiry and router-mode stateful resource routing;
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
evidence that all hosted OpenAI model catalog entries use identical
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
deterministically lowers each `{namespace, name}` pair to a collision-safe
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
checkpoints and intended order, although supporting foundations may land before
their owning phase.

### Phase 0: freeze the oracle and the specification ✅

Purpose: freeze named fork fixtures and a dated OpenAI contract snapshot before
the architectural work begins.

Completed work:

- [x] Build the fork with `ninja -j32` in `build/` and establish focused C++ and
  authenticated HTTP/SDK baselines.
- [x] Pin this roadmap to a dated OpenAI contract snapshot and record inherited
  multimodal tool-result and `item_reference` behavior as compatibility
  requirements.
- [x] Capture deterministic sync and SSE fixtures for stable IDs, tool calls,
  structured output, and SDK decoding.
- [x] Complete PR archaeology without importing the rejected PR's architecture
  wholesale.
- [x] Capture the standalone Codex 0.149.1 create request and tool negotiation
  as a dated, sanitized, deterministic fixture.
- [x] Cover multimodal tool-result continuation and genuinely generated
  parallel calls with deterministic and live evidence.

Gate:

- [x] The existing code builds.
- [x] Named sync/SSE, Codex-request, multimodal-continuation, and generated
  parallel-call fixtures establish the baseline consumed by later phases.

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
- [x] Restore no-longer-needed fork edits to current upstream and keep the
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
- [x] Add a thread-safe `response_store` interface and in-memory implementation
  with a configurable visible-resource cap, revisions, item indexing,
  transition checks, and lineage-aware descendant preservation across public
  deletion or visible-set eviction.
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

### Phase 3: converge on the core OpenAI Responses resource ✅

Purpose: keep the completed native generation/event spine and converge its
advertised HTTP resources on strict OpenAI semantics.

Completed work:

- [x] Implement stable function/custom call identity and validate interleaved
  tool-call ordering with SDK-decodable golden transcripts.
- [x] Implement durable foreground `store`, `previous_response_id`,
  `item_reference`, retrieve, delete, materialized input-item pagination, and
  normalized input-token counting over a versioned SQLite backend. Terminal
  snapshots, item indices, the immutable parent-linked response graph, and
  tombstone-aware continuation survive restart.
- [x] Capture a dated OpenAI `/input_items` oracle and implement the observed
  prior-input, prior-output, current-input lineage, including resolved
  `item_reference` values.
- [x] Migrate SQLite to schema v5: persist immutable direct-input/output nodes
  and nullable tombstones, reserve output-item IDs globally, and write
  payload-v2 rows without a duplicated materialized request. Read old v4
  `detached_context` only as a legacy lineage checkpoint for databases already
  affected by the former hard-delete policy.
- [x] Preserve structured output and request-dependent wire fields through the
  native snapshot/codec layer.
- [x] Persist completed sync and split-frame SSE terminal responses without
  dangling route lifetimes. Stored streams release only complete frames and do
  not expose or cache a success terminal until SQLite commits it.
- [x] Store only each node's direct input/output contribution and materialize
  `/input_items` or continuation by walking retained ancestors. Soft deletion
  hides a response and its item references publicly without copying or revising
  descendants; middle-node deletion, restart, grandchild, and
  delete-during-generation races have deterministic coverage.
- [x] Reject conversation resources and compact while their lifecycle semantics
  remain unavailable. Gate background streaming on canonical resumable event
  history, then advertise it only after that journal and lifecycle landed.
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
  restart, ancestor deletion, and graph-materialized continuation. Unsupported
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
- [x] Implement the non-streaming background core with an owned, joinable
  worker: return an immediately durable `in_progress` resource, allow polling
  and cancellation, reject active deletion with 409, and cancel/join all work
  through a route shutdown hook before backend teardown. Queue termination
  also releases workers waiting for llama-server to leave its idle sleep state.
  Background `store: false` uses the same SQLite resource backing as
  `store: true` because polling requires server-side state. The existing
  llama-server task/slot queue remains the only scheduler.
- [x] Adopt fail-stop restart semantics: atomically mark orphaned active
  snapshots `failed` with `server_restarted` during route startup, without
  attempting to recover model execution or affecting fresh requests. Preserve
  a multi-delta committed journal prefix, append one contiguous terminal failure
  event, and make repeated startup reconciliation idempotent.
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
  classify the observed top-level fields in focused tests.

Completed spine coverage:

- [x] Complete bounded differential/live coverage for late generation errors,
  disconnect cancellation, and genuinely generated interleaved tool calls.
  Deterministic tests cover sync/SSE projection, late failure envelopes, parser
  correction, limits, typed persistence failures, cancellation polling,
  namespace tools, and interleaved function/custom/local-shell calls. A later
  rejected native delta does not retract earlier accepted output: native
  history retains its valid suffix and appends contiguous `error` and
  `response.failed` terminal events. When persistence succeeds, streaming
  journaled sinks checkpoint that combined suffix before exposing it.
  Translation failures terminate from the last successfully projected native
  state, and a poisoned sink accepts only start/failure/cancellation recovery.
  A live disconnected stream emitted an actual server task cancellation and
  released the only slot; a live Qwen stream then generated two distinct
  parallel calls across 88 ordered Responses events.

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
  `reasoning.encrypted_content` is explicitly rejected.
- [x] Accept omitted/null `moderation` and reject non-null values. The
  current public contract does not document a disabled moderation object, so
  inventing one would not be a truthful compatibility feature.
- [x] Normalize and echo `parallel_tool_calls: null` as `true`, matching the
  request default and the generation input consumed by llama-server.

The replay pipeline recognizes 16 named replay/input item shapes:
`message`, three client call shapes, seven client/hosted output shapes,
`reasoning`, two compaction shapes, `ghost_snapshot`, and `item_reference`. It
also recognizes nine named content shapes: `input_text`, `output_text`, `text`,
`reasoning_text`, `refusal`, `input_image`, `computer_screenshot`, `input_file`,
and MCP `image`. Capability expansion beyond those implemented shapes now has
an explicit owner in Phase 4 or Phase 6; it is not an open-ended Phase 3 gate.

#### Completed Phase 3 spine: resumable background SSE

OpenAI's
[background HTTP profile](https://developers.openai.com/api/docs/guides/background)
does not require WebSockets. A client creates with
`background: true, stream: true`, receives the ordinary Responses SSE event
stream, and may reconnect with
`GET /v1/responses/{response_id}?stream=true&starting_after=N`. OpenAI's
[WebSocket mode](https://developers.openai.com/api/docs/guides/websocket-mode)
is a separate, bidirectional, multiplexed transport and remains an optional
Phase 5 profile.

- [x] Add a canonical SQLite event journal for background-stream-capable
  responses. Sequence-numbered events are appended in authoritative order and
  drive both the live SSE subscriber and later replay; the terminal snapshot
  is not asked to reconstruct historical delta boundaries. Each logical event
  is durable before any HTTP subscriber may observe it.
- [x] Accept `background: true, stream: true` on create. Start the owned
  background job before returning SSE, stream journaled events as they arrive,
  and unsubscribe that client—rather than cancel generation—when its HTTP
  connection closes.
- [x] Implement streamed retrieval with `stream=true&starting_after=N` for a
  response originally created with `stream: true`. Replay only events after
  the cursor, then tail the same journal until terminal or persistence error
  without gaps or duplicates. Continue rejecting a new stream for responses
  created with `stream: false`.
- [x] Match the 2026-08-25 OpenAI cursor probe: an equal-tail or future cursor
  waits while its response is active and returns HTTP 200 with an empty body
  once terminal. Malformed and overflowing cursors remain validation errors;
  a missing event inside committed history remains corruption.
- [x] When persistence remains available, project cancellation, generation
  failure, incomplete, and completed outcomes through the same journal. Active
  delete remains 409; terminal delete soft-hides the resource while retaining
  journal and lineage needed by existing descendants. Startup fail-stop
  reconciliation appends a terminal `server_restarted` failure after the
  committed prefix instead of attempting inference recovery. Persistence
  failure itself uses the transport-level fail-stop behavior documented above.
- [x] Add deterministic disconnect/reconnect, cursor, terminal-race,
  cancellation, deletion, and restart tests; add authenticated HTTP/SDK and one
  live Qwen smoke proving that create-stream disconnect does not cancel
  generation and a later retrieval reaches the terminal event. The live
  Qwen3.8-27B client disconnected at sequence 0 while status remained `in_progress`,
  then resumed 325 contiguous events through `response.incomplete`, retrieved
  the identical terminal snapshot, and deleted the resource.

Phase 3 stops at that HTTP/SSE contract. It does not include WebSockets,
compaction, conversations, provider-backed files, hosted-tool execution,
expiry/GC, router-mode resource affinity, or high-availability inference
recovery. Its durability boundary is the single-host SQLite database under a
`llama-server` process restart: acknowledged events and response lineage
survive, but inference is terminalized rather than resumed. Machine or disk
loss, cross-host failover, event-log segmentation, physical tombstone
reclamation, and retention scheduling are operational extensions, not hidden
Phase 3 promises.

Compatibility guidance: preserve fields and correct validation, status, usage,
or event ordering when the dated public contract, an official SDK, or Codex
exposes an observable requirement. Do not turn unobserved schema fields,
byte-for-byte event timing, or hypothetical concurrent access patterns into
work.
New evidence becomes a focused regression and is assigned to the phase which
owns that capability; it does not reopen completed Phase 3 by default.

#### Phase 3 advertised conformance matrix

Rows marked ✅ are advertised by the current sidecar. Each row has native or
resource-level coverage plus official SDK or raw-HTTP coverage appropriate to
its public surface; the background-streaming row additionally has a live
Qwen3.8-27B disconnect/resume proof.

This table is the meaning of "complete advertised matrix" in the Phase 3 gate:
the combinations and behavior named in a ✅ row, not the Cartesian product
of fields in the OpenAI create schema. "SDK round-trip" means that the
official Python sync and async SDK can construct the supported request and
decode its response, event, item, and resource objects without patched local
types. Raw HTTP covers an operation only when the installed SDK has no method
for it.

| Surface | Advertised combinations and behavior | Status |
| --- | --- | --- |
| Foreground create | `background: false`, sync or SSE, with either `store` value; disconnect cancels an active foreground stream. | ✅ |
| Background polling | `background: true, stream: false`, with either `store` value; immediate active resource, retrieve polling, cancel, active-delete conflict, terminal soft deletion. | ✅ |
| Background streaming | `background: true, stream: true`, with either `store` value; create SSE survives subscriber disconnect and `GET ...?stream=true&starting_after=N` replays then tails without gaps, including equal-tail and future cursors. | ✅ |
| Stored resources | Retrieve, soft delete, and `/input_items` for stored or temporarily pollable responses; repeated `include[]` parsing with the seven truthful projections documented above. | ✅ |
| Continuation and references | `previous_response_id`, `item_reference`, deletion-safe graph materialization across tombstoned ancestors, and input-token counting across text and multimodal history. | ✅ |
| Client-executed tools | Function, custom, and negotiated local-shell calls; independent item/call IDs, parallel/interleaved calls, replayed outputs, and mixed text/image/file tool results. | ✅ |
| Typed items and media | Text and reasoning output, messages, replayed refusal content, structured text output, input images/files, and honest incomplete/failed/cancelled terminal objects with usage. | ✅ |
| Cancellation and errors | Official background cancellation plus the documented local foreground extension; stable missing, repeated, terminal, conflict, validation, media/provider, and storage-error envelopes. | ✅ |
| Token count | `/v1/responses/input_tokens` over the same normalized, reference-resolved, media-aware generation input used by create. | ✅ |

Phase 3 explicitly does **not** advertise:

- conversations or `/responses/compact`;
- WebSocket transport;
- server-executed web search, file search, computer, MCP, image-generation, or
  other hosted providers;
- provider-backed `file_id` resolution;
- model-produced typed `refusal` output content;
- direct `input_audio`, model-produced audio/image, or the newer
  `shell_call`/`shell_call_output` and `apply_patch_call`/
  `apply_patch_call_output` replay families which the current catalog does not
  negotiate;
- selectable reasoning-summary policy and `text.verbosity` lowering;
- non-default service tiers, prompt resources, automatic truncation,
  non-null moderation, prompt-cache policy, encrypted reasoning retrieval,
  llama telemetry extensions, or other request knobs rejected by the dated
  validation fixture;
- router-mode stateful resource affinity, expiry/GC policy, externalized blob
  storage, or recovery of interrupted inference after process death.

Completed supporting work:

- [x] Connect native active and terminal snapshots to store compare-and-swap,
  without revising descendants when an ancestor is tombstoned during
  generation.
- [x] Add an aggregate inline-media request budget without changing the
  resource API.
- [x] Validate `user`, `safety_identifier`, and `prompt_cache_key` as
  non-inference identifiers; lower `parallel_tool_calls` to generation; and
  normalize its nullable default consistently in the response echo.

Gate:

- [x] Native generation owns sync and SSE foreground responses for both `store`
  values, including typed lifecycle, tool calls, usage, cancellation, and
  multimodal replay; `store: true` additionally provides persistence,
  continuation, and retrieval.
- [x] The installed sidecar path constructs no Responses events in generic task
  code. Upstream's own limited renderer remains compiled and unchanged for the
  explicit no-extension configuration.
- [x] Non-streaming background create returns a durable active resource and
  supports polling, cancellation, terminal soft deletion, graceful shutdown,
  and fail-stop restart behavior for both `store` values.
- [x] Stored continuation and deletion tests cover text, reasoning, tool calls,
  and multimodal tool outputs across branching lineage and tombstoned
  ancestors. Schema migration covers payload-v1 fixtures and direct v4-to-v5
  legacy checkpoints without using the checkpoint form for new writes.
- [x] Focused fixtures cover malformed, unsupported, missing, conflicting,
  cancelled, failed, incomplete, and storage-error outcomes.
- [x] Official OpenAI Python SDK objects round-trip without local patches
  across the ✅ rows above, including sync and async streaming clients; raw
  HTTP supplements operations the installed SDK does not expose.
- [x] Background create SSE and cursor-based streamed retrieval pass the exact
  journal/disconnect/reconnect lifecycle above through native, HTTP/SDK, and
  live Qwen coverage, with future-cursor semantics pinned by the dated OpenAI
  probe.
- [x] Fixed-size 64/256/1,024-node chains demonstrate approximately linear
  SQLite storage and materialized-view growth, while five-run event-journal
  medians remain in the prior performance class for static and tombstoned
  ancestry.

### Phase 3.5: MVP truthful Codex model capabilities ✅

Purpose: make Codex choose a truthful prompt, context, modalities, tools, and
reasoning policy for the canonical `qwen3.8-27b-local` deployment without
changing public model-list behavior. General multi-model policy is deliberately
deferred to Phase 7.

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

Gate:

- [x] The canonical Qwen deployment is discovered after catalog refresh and
  completes an authenticated Codex Responses turn; a cold pinned-model turn is
  allowed to use client fallback once while refresh populates shared cache.
- [x] Public model-list clients observe the original llama-server response.
- [x] The private catalog advertises only the Qwen MVP's implemented context,
  text/image modalities, shell/patch tools, and `low`/`medium`/`xhigh`
  reasoning efforts. It explicitly disables reasoning summaries and omits
  hosted search.

Catalog metadata is an acceptance criterion of the capability which consumes
it, not a perpetual Phase 3.5 promise. For example, Phase 4 owns hosted-search
advertisement, while Phase 6 owns selectable reasoning-summary and verbosity
semantics. Phase 7 generalizes those already-truthful policies across models.

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
- [ ] Advertise hosted search in the Codex catalog only after its configured
  provider and inference-round loop implement the advertised behavior. Apply
  the same rule independently to each later hosted provider.
- [ ] Add file-search and image-generation providers as separate strategies.
- [ ] Resolve provider-backed `file_id` references through the configured file
  provider; inline/data file and image content remains the provider-free path.
- [ ] Preserve typed media/file results through provider output, storage, prompt
  lowering, and streaming, adding hosted-tool-specific image/audio output
  variants only when a provider can produce them truthfully.
- [ ] Add a deterministic fake for each implemented provider contract.
- [ ] Add timeouts, size limits, approval/permission hooks, maximum rounds, loop
  detection, and audit logging.

Gate:

- [ ] Hosted tool calls can drive multiple inference rounds inside one response.
- [ ] Cancellation interrupts both inference and provider work.
- [x] Missing providers fail closed with compatible errors.
- [ ] Provider failures never corrupt stored response state or event sequences.
- [ ] MCP is one passing adapter, not a special case in the protocol layer.
- [ ] Codex catalog advertisement and the configured hosted-provider registry
  agree for the hosted capabilities exposed to the canonical Qwen deployment.

### Phase 5: compact, conversations, and optional transports 🕒

Purpose: build the next stateful Responses resources on the completed Phase 3
HTTP spine. Alternate transports and deployment profiles remain independent
choices; they do not delay compact or conversations.

Outstanding resources:

- [ ] Implement `/v1/responses/compact` using the same typed item model,
  response store, and model-context policy, with official SDK and live Codex
  coverage.
- [ ] Implement conversation creation/attachment and the companion conversation
  resources needed for an official SDK client to use that lifecycle normally.

Optional profiles, not Phase 5 gates:

- **WebSocket mode.** One persistent `/v1/responses` connection can multiplex
  response streams and accept incremental inputs/tool outputs. It may improve
  long, tool-heavy agent loops, but background create/resume already works over
  HTTP/SSE and does not depend on WebSockets. Implement this only when a client
  or measured latency goal justifies the additional transport.
- **Front-door admission and backpressure.** A public or high-concurrency
  deployment may need a bounded executor which exposes `queued` while a
  background request waits, or rejects excess work. The trusted single-user
  profile uses llama-server's existing task/slot scheduler.
- **High-availability inference recovery.** Resuming interrupted generation
  would require durable sampler/model state, ownership leases, and failover.
  The single-server profile deliberately terminalizes interrupted responses
  and keeps fresh sessions usable.

Gate:

- [ ] Compact produces an SDK-decodable compacted item and supports normal
  continuation without a parallel context representation.
- [ ] Conversation attachment and continuation pass create/retrieve/delete and
  restart coverage without weakening ordinary stored-response semantics.

### Phase 6: remaining capability convergence and hardening 🕒

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

Compatibility evidence policy: add a dated OpenAI probe only when a suspected
observable schema, validation, error, or event-order mismatch cannot be
resolved from the public contract. Commit a sanitized semantic fixture.
Byte-for-byte streams, production timing equivalence, and credentials in the
normal test path are not goals.

Outstanding work:

- [ ] Implement selectable `reasoning.summary` request and event semantics,
  then truthfully enable the corresponding Codex catalog fields for the Qwen
  deployment.
- [ ] Implement `text.verbosity` lowering with an observable local-model policy,
  then truthfully advertise its supported values and default to Codex.
- [ ] Expand Codex black-box suites with scripted generation and live Qwen smoke
  tests beyond the single canonical turn.
- [ ] Fuzz request parsing, SSE framing, replayed items, provider output, and
  cancellation sequences.
- [ ] Soak parallel responses, storage eviction, disconnects, and hosted-tool
  loops across the profiles actually advertised by the deployment.
- [ ] Audit authorization boundaries, SSRF/path traversal, subprocess handling,
  secret redaction, size limits, and denial-of-service surfaces.
- [ ] Document supported, experimental, and unavailable capabilities and
  automate advancement of the OpenAI spec snapshot.

Deferred capability and operations backlog, not Phase 6 gates:

- Add `shell_call`, `shell_call_output`, `apply_patch_call`, and
  `apply_patch_call_output` replay items before a future catalog negotiates
  those native families; the present catalog uses supported local-shell and
  custom-tool forms.
- Add direct `input_audio` only when a deployed llama-server adapter consumes
  it, and model-produced audio/image only when a loaded model exposes that
  output capability truthfully.
- Add concurrent retrieval of active foreground work only if a client needs it;
  background polling and SSE resume already own the asynchronous use case.
- Choose expiry/GC and physical `store: false` cleanup when this deployment
  adopts a retention policy. Add expired-resource fixtures with that policy.
- Add durable byte accounting and external blob storage only if canonical
  SQLite snapshots stop carrying response media directly.

Gate:

- [ ] Newly advertised Phase 4-7 conformance rows pass on their enabled
  transports.
- [ ] Codex completes a broad tool/media/state test corpus against Qwen.
- [x] The installed sidecar has no dependency on generic Responses JSON/event
  logic; the unchanged upstream stock path remains available only when the
  extension is not installed.
- [ ] Replaying the fork's server seam after a representative upstream merge is a
  small, mechanical operation.

### Phase 7: support multiple models and model routing 🕒

Purpose: generalize the completed single-model Qwen endpoint to heterogeneous
models behind one llama-server entry point. This is a deployment and routing
axis, not a prerequisite for completing the Responses protocol against
`qwen3.8-27b-local`, so it is deliberately last.

Completed foundations:

- [x] The model-catalog decorator is a chain-of-responsibility seam which
  preserves the public model list and projects a downstream llama-server
  catalog only for recognized Codex discovery requests.
- [x] Focused tests cover router-shaped catalog projection, while the canonical
  Qwen policy proves the private catalog can differ truthfully from the public
  `/v1/models` object.

Outstanding work:

- [ ] Replace Qwen-alias source special cases with configurable or model-derived
  capability policy so another local model can advertise truthful context,
  modalities, instructions, tools, reasoning levels, and defaults without a
  source change.
- [ ] Project each model's implemented capabilities independently. Phase 7 does
  not invent new reasoning, verbosity, hosted-tool, compact, or transport
  features; it reports the capabilities completed by their owning phases.
- [ ] Add deterministic integration coverage for API prefixes, loaded
  single-model servers, sleeping/cached models, and actual router-process
  handler substitution.
- [ ] Support heterogeneous catalogs in which models expose different context,
  modality, reasoning, and tool policies without leaking one model's defaults
  into another.
- [ ] Route create and input-token-count requests to the selected model, then
  retain process-lifetime `response_id -> child/model` ownership so retrieve,
  input-items, continue, cancel, and delete reach the same model-serving child.
- [ ] Fail closed when router ownership is absent or stale. Persist affinity
  only if a future router profile promises continuity across router restart;
  Phase 7 does not add high-availability inference recovery.
- [ ] Track genuinely required Codex catalog-field changes with tolerant
  fixtures while keeping the locally owned base-instructions policy independent
  of `client_version`.

Gate:

- [ ] At least two materially different local models advertise truthful Codex
  catalogs and complete authenticated Responses turns without source edits.
- [ ] Model selection, input-token counting, continuation, retrieval,
  cancellation, and deletion are routed consistently to the owning child.
- [ ] Loaded, sleeping/cached, prefixed, and router deployments pass
  deterministic integration coverage.
- [ ] The canonical Qwen profile and undecorated public model-list behavior
  remain unchanged.

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
    --ctx-size 98304 \
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

## Open questions to answer with spikes, not guesses

These do not change the architectural decision, but they affect implementation
details:

- Which fields newly observed in a Codex or SDK request are meaningful for
  local inference, truthful metadata/no-ops, or explicit unsupported behavior?
- Where should model-specific opaque reasoning state live so that replay is
  correct but protocol code remains model-neutral?
- Which hosted tools require explicit user approval, and how is that policy
  configured without putting deployment concerns in the request parser?

Each spike should end in a small test or an ADR amendment in this document.

## Next conformance slice

Phase 3 is complete. Ordinary and background creation share typed lowering;
the sidecar owns Responses objects, event ordering, SQLite resources, polling,
cancellation, graph-materialized continuation, soft deletion, and resumable
SSE; and the installed route has no legacy renderer, hidden forwarded request,
or fallback selector. Deterministic native tests, the complete fork-owned
Python SDK suite, and authenticated Qwen live smokes cover the advertised
matrix.

Recommended next checkpoint: implement the first Phase 4 hosted-tool inference
round loop.

1. Make the Responses orchestrator recognize a hosted call, invoke its
   configured C++ provider, append the typed result, and resume inference in the
   same response.
2. Prove multiple rounds, stable item/event identities, provider failure, and
   cancellation with a deterministic fake provider before adding transport.
3. Add MCP adapters for the local terminal and web-search services through the
   existing transport-neutral interface.
4. Preserve provider text/media/file results through SQLite, prompt lowering,
   polling, and SSE without introducing a second item representation.
5. Advertise hosted search to Codex only when the configured provider and round
   loop pass the canonical Qwen path.

WebSockets, compact, conversations, provider-backed file IDs, expiry/GC,
router affinity, and inference recovery retain their later phase owners.
