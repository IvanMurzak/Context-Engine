# cmake/

Shared CMake modules + build-toolchain records for the Context Engine build. Currently:

- `ContextWarnings.cmake` — the `context_warnings` interface target, the warnings-as-errors
  baseline every target links.
- `toolchain-versions.json` — the **L-42 per-target pinned toolchain manifest**: the versioned,
  DECLARED record of target-vs-actual toolchains per platform (strict clang pin on Linux,
  recorded Apple clang on macOS, documented MSVC on Windows with clang-cl adoption staged,
  documented emsdk LLVM on web). Read/enforced by `tools/check_toolchain.py`, applied per CI
  job by the `.github/actions/pinned-toolchain` composite action.

Governed by the Context Engine design records: **DESIGN-DECISIONS.md locks L-38
(sanitizer-gated CI) and L-42 (pinned toolchain manifest)** and **ROADMAP.md §1 M0**.
