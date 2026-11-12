# Linting and upstream merge discipline

Policy clarified: 2026-11-12.

The Responses work is still WIP, but complete enough that we are not actively
developing it at present. We regularly merge upstream so the fork does not
drift. Small differences in upstream files have a recurring maintenance cost.

## Goals

- Keep as light a touch as practical on upstream files.
- Do not introduce problems in our own code or regress upstream behavior.
- When we intentionally touch an upstream file, leave it free of diagnostics
  in VS Code's Problems view under the normal project/editor configuration.
- For new files and code we own, follow upstream's `.clang-tidy` configuration.
  There is no upstream merge friction to justify a weaker standard there.

The first two goals can pull in different directions. Make the smallest change
that preserves correct behavior and keeps the editor useful. A touched upstream
file does not have to satisfy every standalone clang-tidy check just because we
worked on it.

## Upstream files: when in Rome

Preserve upstream's conventions, include ordering, formatting, and use of
transitive includes when they build correctly and do not cause editor Problems.
Keep additions required by our functionality or demonstrated diagnostics. A
general preference for explicit includes is not, by itself, a reason to carry
an upstream diff.

Fix existing Problems in a file we intentionally modify, including diagnostics
in inherited code. Keep those fixes local and behavior-preserving. Do not
reformat the whole file, run automatic include cleanup over it, or expand the
task into cleaning untouched upstream headers. Real correctness defects still
deserve fixes whether or not a linter notices them.

## Code we own

For new code and files owned by this fork, including the Responses module, use
the repository's `.clang-tidy` as the linting standard. Prefer explicit
dependencies and fix applicable diagnostics. Run standalone clang-tidy with
`build/compile_commands.json`; restrict header diagnostics to headers we own or
intentionally changed. Untouched upstream dependencies do not expand the work.

A suppression needs a concrete reason that the diagnostic is inapplicable and
an adjacent comment explaining that reason. Do not suppress real defects or
weaken shared configuration just to obtain a clean result.

## Problems and clang-tidy are different checks

VS Code's Problems view shows diagnostics published by its configured tools.
In our current setup, C++ diagnostics come from clangd. It runs supported
clang-tidy checks, but its diagnostics are not identical to running the
standalone `clang-tidy` executable with the repository configuration.

For example, `.clang-tidy` enables `misc-include-cleaner` through `misc-*`.
Clangd explicitly disables that tidy check and uses its own include analysis.
Its `Diagnostics.MissingIncludes` setting defaults to `None`. Enabling
`MissingIncludes: Strict` would expose additional missing-direct-include
Problems; it is not enabled in our current editor configuration.

This explained the `server.cpp` investigation: removing `<cstdio>` still
compiled because `common.h` includes it. Standalone clang-tidy warned about
`fprintf`, while the editor showed no additional Problem. The existing three
Problems concerned braces, a method that could be `const`, and an unnamed
parameter. Those editor diagnostics were the cleanup target.

Do not infer that an editor is clean from a successful build, or that a touched
upstream file must be standalone-tidy clean because its editor is clean. Check
the actual tool and configuration used, and state what was verified. Changing
diagnostic settings is a separate policy decision, not part of making a file
appear clean.

References: [clangd configuration](https://clangd.llvm.org/config.html#diagnostics),
[clangd's exclusion of the tidy include-cleaner check](https://github.com/llvm/llvm-project/blob/llvmorg-21.1.8/clang-tools-extra/clangd/TidyProvider.cpp#L205-L206).
