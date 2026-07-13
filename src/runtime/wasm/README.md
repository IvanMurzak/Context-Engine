# `src/runtime/wasm/` — sandboxed WASM package-migration runner tier (issue #71)

The runtime home of the **sandboxed WASM VM tier** that executes package-shipped migration steps
(`MigrationTier::package_sandboxed`) — turning the honest `migration.runner_unavailable` refusal in
`src/editor/migrate/migrate_document.cpp` into a real, deterministic, sandboxed runner. Runtime =
**wasmtime-Cranelift** (owner ruling 2026-07-12), whose per-instruction `consume_fuel` metering is
what L-37 needs (deterministic budget enforcement, no wall-clock limits).

## Status — PR 1 of 4 (this leg is INERT)

The #71 work lands as a single-lane 4-PR chain. **This directory today carries only PR 1 — the
inert wasmtime supply-chain leg. It executes no wasm and links into nothing that runs.**

1. **PR 1 — supply chain (inert, THIS PR).** `tools/wasmtime-prebuilt.json` (pin manifest) +
   `tools/fetch_wasmtime.py` (TLS + SHA-256 pin + verify-before-use, fail-closed + offline
   `--source` + idempotency stamp — the 1:1 sibling of `tools/fetch_v8.py`) + the `context_wasm`
   CMake INTERFACE target here, which fetches + exposes the C-API (headers + lib). Fetcher tests in
   `tools/tests/test_fetch_wasmtime.py`. **No wasm runs; `context_wasm` links into nothing.**
2. **PR 2 — MigrationRunner seam + guest ABI.** The runner interface in `src/editor/migrate/` (the
   migrate lib does NOT link the runtime); `apply_step()` routes `package_sandboxed` to an injected
   runner, else the prior honest refusal. Freezes the zero-import guest ABI.
3. **PR 3 — WasmRunner on wasmtime.** The deterministic config (`consume_fuel`, NaN canonicalization,
   relaxed-SIMD off, threads off, fixed linear-memory limit), `fuel = K × budget.max_nodes`, the
   catalog codes (fuel-trap → `migration.budget_exceeded`, trap → `migration.step_failed`), and the
   R-FILE-005 cold-start slot. **This is where `context_wasm` is first linked + `CONTEXT_BUILD_WASM`
   flipped ON in the runner's CI path.**
4. **PR 4 — determinism gate + fixtures.** Committed `.wasm` fixtures + fail-closed tests + the
   cross-OS determinism ctest with fuel-parity; `set_hash()` hashes the wasm module CONTENTS.

## Build gating — a CI-only dependency path

`context_wasm` is gated behind **`CONTEXT_BUILD_WASM` (default OFF)**, exactly like V8
(`src/runtime/js/`), wgpu-native (`src/render/`), and CEF (`src/editor/cef/`): the wasmtime C-API
prebuilt is an **MSVC/Clang-ABI** archive the local Strawberry-GCC Windows `dev` gate cannot link.
With the option OFF — the default 3-OS build matrix and the local dev gate — this subdirectory
early-returns (no fetch, no target), so the routine gate stays fast and green.

The pins are already CI-proven on all three OS by the existing **`spike-wasm`** job (`spikes/wasm/`
uses the SAME wasmtime 46.0.1 C-API prebuilt + SHA-256 pins), and `test_fetch_wasmtime.py` exercises
the fetcher + validates the real pin manifest in the **`python-tests`** job.

## Supply chain — third-party prebuilt, not the first-party trust root

The wasmtime C-API prebuilt is a **third-party** artifact authenticated by its own publisher over
TLS + a SHA-256 pin (`docs/signing.md` — the wgpu-native / CEF / rusty_v8 precedent). It is
explicitly **out of scope for the engine R-SEC-009 first-party trust root**, so it is deliberately
NOT routed through `tools/verify_artifact.py`. A from-source owned vcpkg wasmtime port (a full Rust
toolchain) is a separate pre-1.0 hardening task, exactly as for V8.
