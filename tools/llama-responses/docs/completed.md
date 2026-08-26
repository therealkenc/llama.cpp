# Llama Responses completed work

This is the historical ledger for landed Responses work. It records the
observable compatibility baseline, implementation evidence, completed phases,
and foundations already available to later phases. Active work belongs in
[`sprint.md`](sprint.md); stable design and operating notes belong in
[`notes.md`](notes.md).

Checkpoint through Phase 3.6: 2026-08-26

## Compatibility baseline at the Phase 3 checkpoint

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

## Completed delivery phases

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

### Phase 3.6: deferred namespace loading, tool search, and Responses Lite ✅

Purpose: keep Codex's installed plugin catalog out of Qwen's initial context
while implementing the typed discovery round trip and the compact request shape
used by current GPT-5.6 Sol/Terra/Luna catalog entries.

Completed protocol work:

- [x] Implement client-executed `tool_search`, including validation,
  collision-safe Chat-template lowering, native generation items and IDs,
  object-valued arguments, status, call correlation, and the exact generic
  output-item SSE lifecycle observed from OpenAI.
- [x] Accept deferred function/custom and mixed namespace members. Withhold
  deferred schemas from the first model prompt while retaining searchable
  source descriptions; preserve the eager namespace path when search is not
  declared.
- [x] Validate and lower `tool_search_output.tools`, inject only selected
  definitions, preserve namespaced public identity, and reject malformed,
  mismatched, duplicate, or conflicting lineage before generation.
- [x] Store and replay search call/output items through response snapshots,
  input-item retrieval, `previous_response_id`, process restart, and
  `store: false` complete manual replay without a parallel search-state store.
- [x] Implement the Codex Responses Lite request shape: the internal header,
  developer-role `additional_tools`, the following developer instruction
  message, `parallel_tool_calls: false`, and
  `reasoning.context: "all_turns"`.
- [x] Preserve Lite declarations as `at_` input items, share ordinary tool and
  instruction lowering, deduplicate semantic copies independent of JSON key
  order, merge compatible selected namespace members, and project the active
  tools in create/retrieve envelopes.
- [x] Match OpenAI's choice behavior: search-only `required` is invalid; when
  a direct tool also exists, `required` is accepted and may resolve to either
  the direct tool or client search; direct named forcing of search is invalid.
- [x] Advertise `supports_search_tool: true` and
  `use_responses_lite: true` independently for the canonical Qwen model. Keep
  the private catalog independent of the exact `client_version` value and
  preserve undecorated `/v1/models` downstream behavior.

Conformance and live evidence:

- [x] Add a loopback-only, streaming-safe redacting proxy with ephemeral
  authentication, HTTPS-upstream enforcement, bounded structural JSONL, and
  tests proving credentials and content do not enter telemetry.
- [x] Probe current OpenAI client search in synchronous and streaming modes.
  Pin the no-`name` `tool_search_call` fields, object arguments, call-ID
  behavior, selected-output shape, and event ordering in a sanitized dated
  oracle.
- [x] Capture a fresh Codex 0.149.1 Lite request: 32,390 bytes, no top-level
  tools/instructions, ordered `additional_tools` then developer text,
  `stream: true`, `store: false`, disabled parallel calls, and
  `reasoning.context: "all_turns"`.
- [x] Proxy an isolated Codex 0.149.1 exact-`OK` turn to GPT-5.6 Sol without a
  production ChatGPT session token. The 34,407-byte Lite POST completed with
  6,839 input and 5 output tokens; first byte was 1.705 seconds and total time
  2.524 seconds.
- [x] Complete an authenticated Qwen exact-`OK` turn at 11,648 input tokens,
  then a real Chrome search/selection/execution turn under the 98,304-token MTP
  profile. The Chrome turn made one search call, never truncated, and grew from
  11,688 to 29,650 input tokens—well under the 40K/55K gates and far below the
  prior 88,439-token eager first prompt.
- [x] Pass native validation/lowering/generation/route tests, the complete
  fork-owned official-SDK Python suite, proxy security tests, touched-file
  clang-tidy, and `ninja -j32` in `build/`.

The supported Phase 3.6 profile is current Codex's one-snapshot-at-the-head
HTTP replay shape. Repeated exact snapshots are safe and conflicts are rejected.
No claim is made that arbitrary hand-written histories can retroactively change
which tools were visible before each historical item. Hosted/server-executed
search remains Phase 4 work.

## Foundations already landed for later phases

These completed pieces remain useful, but do not by themselves complete their
owning phases.

### Phase 4 foundations

- [x] Transport-neutral hosted-tool provider contracts, capability reporting,
  progress/cancellation hooks, registry, and fail-closed stubs.
- [x] Provider transport is independent of the public Responses item model;
  MCP remains an adapter option rather than the architecture.

### Phase 6 foundations

- [x] Focused domain, server-conversion, authenticated HTTP/SDK, and live
  Qwen/Codex coverage.
- [x] Touched-file clang-tidy policy for Responses-owned code.
- [x] A small, mechanically reviewable fork-owned server seam.
- [x] Unmodified upstream stock Responses behavior when the sidecar extension
  is not installed.
- [x] Dated OpenAI probes for ambiguous observable behavior, with sanitized
  fixtures committed where useful.

### Phase 7 foundations

- [x] Chain-of-responsibility Codex model-catalog decoration which preserves
  the public model list.
- [x] Router-shaped catalog projection tests and a truthful canonical
  `qwen3.8-27b-local` policy.

## Historical live verification

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

That tool turn also exposed the deployment-pressure defect which Phase 3.6
later closed: the eager Codex prompt/tool catalog began around 86.7K tokens
inside the 90,112-token slot. Codex compacted twice and Qwen unnecessarily
repeated `pwd` before the final answer. A temporary larger Q8-KV profile made
the protocol test possible; client search plus Responses Lite subsequently
reduced the first prompt to roughly 11.7K and completed Chrome under the
98,304-token MTP profile.
