# src/ — engine source and build root

Every CMake/preset/lint file is here; run build and ctest presets from this folder.

- `kernel/` — the microkernel: ~6 stable interfaces (World/ECS, scheduler, module registry, event bus, resources, platform seam)
- `common/` — shared cross-cutting infra (hardened subprocess/temp-file runner)
- `editor/` — **EditorKernel** and everything editor-side (see `editor/AGENTS.md`)
- `runtime/` — **RuntimeKernel**: session/determinism, save, V8/TS, WASM, profiler, netsync (`runtime/AGENTS.md`)
- `render/` — tiered RHI, extract, present path, opt-in GPU backends (`render/AGENTS.md`)
- `packages/` — first-party feature packages (`packages/AGENTS.md`)
- `cli/` — the `context` CLI, a pure projection of the one contract registry (CLI ≡ RPC ≡ MCP ≡ introspection)
- `testing/` — fault-injection harness; `tests/integration/` — milestone exit gates, samples-corpus, game smokes, the editor-session drill; `tests/concurrency/` — co-edit/merge concurrency tests

New engine features go in a package, not the kernel. Each module keeps the `include/` + `src/` +
`tests/` + `CMakeLists.txt` shape of its neighbours and is wired in by an `add_subdirectory` in
`src/CMakeLists.txt` (`editor/`, `runtime/`, `packages/` have no intermediate CMake list; `render/` does).

## C++ rules

- **Warnings-as-errors** via the `context_warnings` INTERFACE target (`../cmake/ContextWarnings.cmake`:
  MSVC `/W4 /WX`, else `-Wall -Wextra -Wpedantic -Werror`). Every new target links it `PRIVATE`
  (exemption: CEF-linking targets, see `editor/AGENTS.md`).
- Format: `.clang-format` (LLVM base, 100 columns, Allman braces, 4-space indent, `PointerAlignment: Left`).
- Recurring CI-only breaks the local GCC gate cannot see:
  - MSVC rejects raw C stdio (`fopen`, `sprintf`, …) as C4996 — use C++ streams.
  - Apple clang/libc++ is stricter on overloads (`std::to_string` on a `std::chrono` rep) — cast to a concrete integer first.
  - Clang warns by default on unused const variables / lambda captures / private fields; GCC does not — hand-audit them.
  - MSVC flags shadowed locals (C4456); GCC `-Wall -Wextra` does not.
  - Non-Windows `#if` branches (`__linux__`, `__APPLE__`, POSIX `#else`) get no local signal — hand-check before pushing.
  - Never return a user-defined C++ type by value across an `extern "C"` boundary (Clang `-Wreturn-type-c-linkage`, MSVC C4190).
