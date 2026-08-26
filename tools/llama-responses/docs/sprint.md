# Llama Responses sprint

Status: Phase 3, the single-model Codex catalog MVP, and Phase 3.6 deferred
namespace loading/Responses Lite are complete. The next feature slice has not
yet been selected.

Stable architecture and operating commands are in [`notes.md`](notes.md).
Completed phases and evidence are in [`completed.md`](completed.md).

Last reorganized: 2026-08-26

## Working agreement

- Keep the current phase detailed enough to carry design decisions, experiments,
  evidence, and follow-ups across turns.
- Move landed work to `completed.md` at a checkpoint instead of accumulating
  checked boxes here.
- Move durable architectural or operational conclusions to `notes.md`.
- Advertise a capability only after its implementation, catalog policy, and
  appropriate deterministic or live evidence agree.
- Add dated OpenAI probes only when public documentation and SDK behavior do
  not settle an observable compatibility question.

## Working area

Record the current slice's design, decisions, measurements, and newly discovered
work here. Keep these notes until the slice lands; then promote the durable
parts to `notes.md` or `completed.md` rather than deleting the reasoning.

## Delivery roadmap

### Phase 3.6: deferred namespace loading, tool search, and Responses Lite ✅

Client-executed deferred discovery and Codex's compact Lite request shape are
complete for the canonical single-model Qwen profile. Typed protocol behavior,
the exact supported scope, conformance evidence, and before/after context
measurements are recorded in [`completed.md`](completed.md).
Stable wire and lowering decisions are recorded in
[`notes.md`](notes.md#codex-namespace-loading-tool-search-and-responses-lite).

Hosted `execution: "server"` search remains Phase 4 work. Arbitrary
hand-authored histories which place conflicting `additional_tools` snapshots
at several positions are rejected; current Codex's one-snapshot-at-the-head
HTTP replay is the advertised and tested Lite profile.

### Phase 4: execute hosted tools through providers 🚧

Purpose: support the hosted-tool part of the real Responses contract without
coupling the API to one deployment.

Completed foundations are recorded in [`completed.md`](completed.md).

Outstanding work:

- [ ] Implement hosted/server-executed tool search for `execution: "server"`,
  including search policy, the null-call-ID call/output pair, selected-tool
  injection, and same-response continuation. Reuse Phase 3.6's typed items and
  lowering; do not route Codex's client-executed discovery through a provider.
- [ ] Implement execution policy and the hosted-tool inference-round loop.
- [ ] Start with MCP adapters for the locally available terminal and web-search
  services while keeping the interfaces transport-neutral.
- [ ] Advertise hosted web search in the Codex catalog only after its configured
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

Completed foundations are recorded in [`completed.md`](completed.md).

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
- [ ] Replaying the fork's server seam after a representative upstream merge is a
  small, mechanical operation.

### Phase 7: support multiple models and model routing 🕒

Purpose: generalize the completed single-model Qwen endpoint to heterogeneous
models behind one llama-server entry point. This is a deployment and routing
axis, not a prerequisite for completing the Responses protocol against
`qwen3.8-27b-local`, so it is deliberately last.

Completed foundations are recorded in [`completed.md`](completed.md).

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

The completed Phase 3/3.5/3.6 spine now covers stateful Responses resources,
truthful single-model Codex discovery, compact deferred-tool negotiation, and a
real Chrome plugin round trip. Deterministic native tests, the complete
fork-owned Python SDK suite, sanitized OpenAI/Codex oracles, and authenticated
Qwen live smokes cover the advertised matrix.

The natural next compatibility choices are now feature work rather than spine
repair:

1. Phase 5 `/v1/responses/compact`, because a long-running Codex session is
   likely to encounter compaction before it needs a server-hosted provider; or
2. Phase 4's hosted-provider inference loop, if local web/terminal services
   should be reachable by non-Codex Responses clients next.

Conversations, WebSockets, provider-backed file IDs, expiry/GC, router affinity,
multi-model policy, and inference recovery retain their later owners.
