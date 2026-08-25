# Codex base instructions

`codex-base-instructions.md` is the single deployment baseline embedded in the
local model catalog. Its initial contents are an unmodified copy of the Codex
CLI fallback base instructions from OpenAI Codex `rust-v0.148.0`, commit
`3ba0f711642a888aec92a611a3f3b2211157ff89`.

Source:
[`codex-rs/models-manager/prompt.md`](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/models-manager/prompt.md)

Imported SHA-256: `ac8ae107a0d72fe3476b430afb161ea4e67da2e446d778aefc44828160559807`

The commit and hash are source provenance, not a compatibility key.
`client_version` never selects a prompt, and this directory is deliberately not
a version registry. The file may evolve as one locally owned, eval-driven
baseline without changing routing policy. CMake regenerates the private C++
header whenever it changes.

Copyright 2025 OpenAI. Licensed under the Apache License 2.0; see
[`licenses/LICENSE-openai-codex`](../../../licenses/LICENSE-openai-codex). The
upstream notice is preserved in
[`licenses/NOTICE-openai-codex`](../../../licenses/NOTICE-openai-codex).
