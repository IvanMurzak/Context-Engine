# src/runtime/wasm/ — the sandboxed WASM package-migration runner tier (issue #71 / L-37 / L-62 /
# R-SEC-009). This is PR 1 of the owner-approved 4-PR #71 chain and is INTENTIONALLY INERT: it
# ACQUIRES + EXPOSES the SHA-pinned wasmtime C-API prebuilt as the `context_wasm` target, but links
# it into NOTHING that runs and executes NO wasm. The deterministic WasmRunner that actually links +
# runs wasmtime (budget->fuel, catalog codes, cold-start slot) lands in PR 3; the MigrationRunner
# seam + guest ABI in src/editor/migrate/ lands in PR 2. This subdirectory does NOT touch
# src/editor/migrate/ — the honest `migration.runner_unavailable` refusal there stays untouched.
#
# Supply chain (owner ruling 2026-07-12, memory context-engine-71-wasm-runtime-ruling.md): runtime =
# wasmtime-Cranelift via a SHA-pinned prebuilt C-API archive (tools/wasmtime-prebuilt.json +
# tools/fetch_wasmtime.py — TLS + SHA-256 pin + verify-before-use, fail-closed; the same third-party-
# prebuilt precedent as V8/rusty_v8, wgpu-native, and CEF in docs/signing.md — NOT the first-party
# R-SEC-009 trust root, so NOT via tools/verify_artifact.py). The CMake forwards the manifest PATH to
# fetch_wasmtime.py (it does NOT parse the pins itself, so they are never duplicated) and consumes the
# flattened <dest>/include + <dest>/lib the fetcher stages — exactly how src/runtime/js/ consumes
# tools/fetch_v8.py.
#
# CI-only dependency path, exactly like V8 (src/runtime/js/), wgpu-native (src/render/), and CEF
# (src/editor/cef/): the wasmtime C-API prebuilt is an MSVC/Clang-ABI archive the local
# Strawberry-GCC Windows `dev` gate cannot LINK, so this leg is GATED behind CONTEXT_BUILD_WASM
# (default OFF). With it OFF — the default 3-OS build matrix AND the local dev gate — this
# subdirectory early-returns: it fetches nothing and creates no target, so the routine gate stays
# fast + green and never attempts an un-linkable MSVC-ABI prebuilt. The pins themselves are already
# CI-proven on all three OS by the existing `spike-wasm` job (spikes/wasm/, the SAME wasmtime 46.0.1
# C-API prebuilt + SHA-256 pins), and tools/tests/test_fetch_wasmtime.py exercises the fetcher +
# real manifest in the `python-tests` job. PR 3 flips CONTEXT_BUILD_WASM ON where the runner links it.

option(CONTEXT_BUILD_WASM
    "Build the sandboxed WASM migration-runner tier's wasmtime C-API supply-chain leg \
(src/runtime/wasm/, issue #71) — fetches + SHA-verifies the pinned wasmtime C-API prebuilt and \
exposes it as the context_wasm target. A CI-only dependency path like V8 / wgpu-native / CEF (the \
MSVC/Clang-ABI prebuilt cannot link under the local Strawberry-GCC dev gate); OFF by default so the \
default matrix + local dev gate never fetch wasmtime. Flipped ON by the WasmRunner (PR 3 of #71)." OFF)

if(NOT CONTEXT_BUILD_WASM)
    return()
endif()

# --- resolve the host wasmtime C-API platform key (matches the manifest + the wasm spike) ---------
set(_ctx_wasm_plat "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_wasm_plat "x86_64-windows")
elseif(APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ctx_wasm_plat "aarch64-macos")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_wasm_plat "x86_64-linux")
else()
    message(FATAL_ERROR "context_wasm: no pinned wasmtime C-API prebuilt for this platform "
                        "(${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}) with CONTEXT_BUILD_WASM=ON. "
                        "Supported: x86_64-linux / aarch64-macos / x86_64-windows.")
endif()

# --- acquire + verify the pinned wasmtime C-API prebuilt (verify-before-use, fail-closed) ---------
# FindPython3 resolves the real interpreter path (incl. the `py` launcher on Windows), robust to the
# python3-vs-py alias difference on the self-hosted Windows runner — same as src/runtime/js/.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_ctx_wasm_manifest "${CMAKE_SOURCE_DIR}/../tools/wasmtime-prebuilt.json")
set(_ctx_wasm_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_wasmtime.py")
set(_ctx_wasm_dest "${CMAKE_BINARY_DIR}/_wasmtime/${_ctx_wasm_plat}")
message(STATUS "context_wasm: fetching + verifying pinned wasmtime C-API prebuilt for ${_ctx_wasm_plat}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_ctx_wasm_fetch}"
            --manifest "${_ctx_wasm_manifest}"
            --platform "${_ctx_wasm_plat}"
            --dest "${_ctx_wasm_dest}"
    RESULT_VARIABLE _ctx_wasm_rc
    OUTPUT_VARIABLE _ctx_wasm_out
    ERROR_VARIABLE _ctx_wasm_err)
if(NOT _ctx_wasm_rc EQUAL 0)
    message(FATAL_ERROR
        "context_wasm: wasmtime C-API fetch/verify FAILED (rc=${_ctx_wasm_rc}) — refusing to build "
        "against an unverified wasmtime (R-SEC-009 fail-closed).\nstdout: ${_ctx_wasm_out}\nstderr: "
        "${_ctx_wasm_err}")
endif()
string(STRIP "${_ctx_wasm_out}" _ctx_wasm_out)
message(STATUS "context_wasm: ${_ctx_wasm_out}")

# Staged layout (tools/fetch_wasmtime.py flattens the archive's <root>/include + <root>/lib):
#   <dest>/include/wasmtime.h (+ wasm.h + wasmtime/*)   <dest>/lib/<platform lib>
# The lib filename follows the wasmtime release convention (static libwasmtime.a on unix; the
# wasmtime.dll import lib on Windows) — matches spikes/wasm/CMakeLists.txt's proven link choice.
if(WIN32)
    set(_ctx_wasm_libfile "${_ctx_wasm_dest}/lib/wasmtime.dll.lib")
else()
    set(_ctx_wasm_libfile "${_ctx_wasm_dest}/lib/libwasmtime.a")
endif()

# context_wasm — the INTERFACE seam a FUTURE WasmRunner (PR 3) links. It carries the SYSTEM header
# dir (so wasmtime's own C headers never trip our -Werror / /WX baseline) + the platform lib + the
# platform system libs the wasmtime C-API embedding docs require. The link recipe MIRRORS the
# proven spikes/wasm/CMakeLists.txt (wasmtime 46.0.1): on Windows link the wasmtime.dll IMPORT lib
# (the DLL carries the runtime's Win32 deps — the PR-3 consumer stages wasmtime.dll next to itself);
# on unix link the STATIC libwasmtime.a + Threads + dl (+ libm on Linux) per the C-API docs. NOTHING
# links context_wasm yet — it is created here so PR 3 slots the runner in without re-deriving the
# supply chain. NO wasm executes in this PR.
add_library(context_wasm INTERFACE)
target_include_directories(context_wasm SYSTEM INTERFACE "${_ctx_wasm_dest}/include")
target_link_libraries(context_wasm INTERFACE "${_ctx_wasm_libfile}")
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(context_wasm INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
    if(UNIX AND NOT APPLE)
        target_link_libraries(context_wasm INTERFACE m)
    endif()
endif()
