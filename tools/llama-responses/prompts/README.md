# Codex prompt snapshots

`codex-0.148.0.md` is an unmodified copy of the Codex CLI fallback base
instructions from OpenAI Codex `rust-v0.148.0`, commit
`3ba0f711642a888aec92a611a3f3b2211157ff89`.

Source:
[`codex-rs/models-manager/prompt.md`](https://github.com/openai/codex/blob/3ba0f711642a888aec92a611a3f3b2211157ff89/codex-rs/models-manager/prompt.md)

SHA-256: `ac8ae107a0d72fe3476b430afb161ea4e67da2e446d778aefc44828160559807`

This proves the fallback behavior of that Codex CLI release; it is not a claim
that every model returned by OpenAI's hosted model catalog uses the same
instructions. `llama-responses` embeds these exact bytes as the current
deployment baseline so successful remote catalog discovery does not replace
Codex's known fallback with a weaker prompt. CMake regenerates the private C++
header whenever this Markdown file changes.

Copyright 2025 OpenAI. Licensed under the Apache License 2.0; see
[`licenses/LICENSE-openai-codex`](../../../licenses/LICENSE-openai-codex). The
upstream notice is preserved in
[`licenses/NOTICE-openai-codex`](../../../licenses/NOTICE-openai-codex).
