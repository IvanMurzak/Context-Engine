# src/runtime/wasm/ — the sandboxed WASM package-migration runner tier (issue #71 / L-37 / L-62 /
# R-SEC-009). PR 1 of the owner-approved 4-PR #71 chain landed the supply chain (the SHA-pinned
# wasmtime C-API prebuilt exposed as `context_wasm`); PR 2 froze the MigrationRunner seam + guest
# ABI in src/editor/migrate/; THIS is PR 3: the deterministic `context_wasm_runner` library — the
# concrete WasmRunner (Cranelift, consume_fuel, NaN canonicalization, relaxed-SIMD off/
# deterministic, threads off, fixed memory limit, fresh Store+instance per step; budget->fuel =
# kWasmFuelPerBudgetNode x max_nodes) — plus its ctest suite (`wasm-runner-*`). The committed
# .wasm fixture corpus + the full cross-OS determinism gate are PR 4.
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
# fast + green and never attempts an un-linkable MSVC-ABI prebuilt. The dedicated 3-OS `wasm-runner`
# CI job (.github/workflows/ci.yml — the cef-substrate pattern) configures with
# -DCONTEXT_BUILD_WASM=ON and is the AUTHORITATIVE gate for this tier. The pins themselves are
# additionally CI-proven by the `spike-wasm` job (spikes/wasm/, the SAME wasmtime 46.0.1 C-API
# prebuilt + SHA-256 pins), and tools/tests/test_fetch_wasmtime.py exercises the fetcher + real
# manifest in the `python-tests` job.
#
# With CONTEXT_BUILD_WASM=ON on a toolchain that CANNOT link the prebuilt (Windows + MinGW/GCC —
# the local dev host; wasmtime.dll.lib is an MSVC import lib), the runner builds its honest STUB
# backend instead (src/wasm_runner_stub.cpp — WasmRunner::create() refuses, so embedders fall back
# to the seam's migration.runner_unavailable refusal; the context_js stub split). Nothing is
# fetched in stub mode.

option(CONTEXT_BUILD_WASM
    "Build the sandboxed WASM migration-runner tier (src/runtime/wasm/, issue #71): fetch + \
SHA-verify the pinned wasmtime C-API prebuilt (context_wasm) and build the deterministic \
WasmRunner (context_wasm_runner) + its wasm-runner-* ctests. A CI-only dependency path like V8 / \
wgpu-native / CEF (the MSVC/Clang-ABI prebuilt cannot link under the local Strawberry-GCC dev \
gate — there the runner builds its honest stub backend); OFF by default so the default matrix + \
local dev gate never fetch wasmtime. The dedicated 3-OS wasm-runner CI job flips it ON." OFF)

if(NOT CONTEXT_BUILD_WASM)
    return()
endif()

# Escape hatch: force the stub even where wasmtime could link (smoke the stub path on a capable
# host — mirrors CONTEXT_JS_FORCE_STUB).
option(CONTEXT_WASM_FORCE_STUB
    "Build the WasmRunner stub backend even where the wasmtime prebuilt could link" OFF)

# CI guard: a leg that MUST exercise the real VM sets this so a silent capability downgrade to the
# stub (wrong toolchain on a CI runner) fails the configure instead of vacuously passing the tests.
option(CONTEXT_WASM_REQUIRE_RUNTIME
    "FATAL_ERROR if the resolved WasmRunner backend is the stub (CI legs gating the real VM)" OFF)

# --- resolve the host wasmtime C-API platform key + link capability ------------------------------
# Platform keys match the manifest + the wasm spike. Windows requires MSVC: wasmtime.dll.lib is an
# MSVC import library the MinGW/Strawberry-GCC dev toolchain cannot link (setup.md § Preconditions
# — the V8 pattern); linux-x64 GCC/Clang and macos-arm64 AppleClang both link the static
# libwasmtime.a (CI-proven by the spike-wasm job).
set(_ctx_wasm_plat "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    if(MSVC)
        set(_ctx_wasm_plat "x86_64-windows")
    endif()
elseif(APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ctx_wasm_plat "aarch64-macos")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_wasm_plat "x86_64-linux")
endif()

set(_ctx_wasm_runtime ON)
if(_ctx_wasm_plat STREQUAL "" OR CONTEXT_WASM_FORCE_STUB)
    set(_ctx_wasm_runtime OFF)
endif()
if(NOT _ctx_wasm_runtime AND CONTEXT_WASM_REQUIRE_RUNTIME)
    message(FATAL_ERROR "context_wasm_runner: CONTEXT_WASM_REQUIRE_RUNTIME=ON but the resolved "
                        "backend is the STUB (platform ${CMAKE_SYSTEM_NAME}/"
                        "${CMAKE_SYSTEM_PROCESSOR}, MSVC=${MSVC}, "
                        "force_stub=${CONTEXT_WASM_FORCE_STUB}). This CI leg must gate the real "
                        "VM — fix the runner/toolchain instead of shipping a vacuous pass.")
endif()

if(_ctx_wasm_runtime)
    # --- acquire + verify the pinned wasmtime C-API prebuilt (verify-before-use, fail-closed) ----
    # FindPython3 resolves the real interpreter path (incl. the `py` launcher on Windows), robust
    # to the python3-vs-py alias difference on the self-hosted Windows runner — same as
    # src/runtime/js/.
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

    # context_wasm — the supply-chain INTERFACE seam (PR 1) the WasmRunner links. It carries the
    # SYSTEM header dir (so wasmtime's own C headers never trip our -Werror / /WX baseline) + the
    # platform lib + the platform system libs the wasmtime C-API embedding docs require. The link
    # recipe MIRRORS the proven spikes/wasm/CMakeLists.txt (wasmtime 46.0.1): on Windows link the
    # wasmtime.dll IMPORT lib (the DLL carries the runtime's Win32 deps — each consumer stages
    # wasmtime.dll next to itself, see the test loop below); on unix link the STATIC libwasmtime.a
    # + Threads + dl (+ libm on Linux) per the C-API docs.
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
else()
    message(STATUS "context_wasm_runner: STUB backend (no linkable wasmtime prebuilt for this "
                   "toolchain — platform ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}, "
                   "MSVC=${MSVC}, force_stub=${CONTEXT_WASM_FORCE_STUB}); nothing fetched, no wasm "
                   "executes. The 3-OS wasm-runner CI job gates the real VM.")
endif()

# --- context_wasm_runner — the deterministic WasmRunner (issue #71 PR3) --------------------------
# Implements the src/editor/migrate MigrationRunner seam (PUBLIC link: the public header exposes
# the seam types), backed by wasmtime through context_wasm (PRIVATE: wasmtime never leaks into
# consumers' include paths). Stub backend where the prebuilt cannot link (above).
if(_ctx_wasm_runtime)
    set(_ctx_wasm_runner_src src/wasm_runner.cpp)
else()
    set(_ctx_wasm_runner_src src/wasm_runner_stub.cpp)
endif()
add_library(context_wasm_runner STATIC ${_ctx_wasm_runner_src})
target_include_directories(context_wasm_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(context_wasm_runner PUBLIC context_migrate)
target_link_libraries(context_wasm_runner PRIVATE context_warnings)
if(_ctx_wasm_runtime)
    target_link_libraries(context_wasm_runner PRIVATE context_wasm)
    target_compile_definitions(context_wasm_runner PRIVATE CONTEXT_WASM_HAS_RUNTIME)
endif()

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern), names
# `wasm-runner-*` — the regex the dedicated CI job runs. In REAL mode the failure-mode guests are
# tiny WAT modules assembled at runtime via wasmtime_wat2wasm (the C-API ships the .wat frontend);
# the happy-path DETERMINISM gate drives the COMMITTED fixtures/migrate_hp.wasm (issue #71 PR4) off
# disk via CONTEXT_WASM_FIXTURE_DIR. In STUB mode only the core test builds, asserting the stub's
# honest refusal. Test names deliberately avoid the install/setup/update/patch tokens (setup.md —
# Windows UAC installer detection). NOTE: a NEW wasm-runner-* test executable must ALSO be added to
# the `wasm-runner` CI job's hand-maintained `cmake --build ... --target` list in
# .github/workflows/ci.yml, or it reports "Not Run" -> RED (the hardcoded-target caveat).
if(_ctx_wasm_runtime)
    set(_ctx_wasm_tests
        test_wasm_runner
        test_wasm_fuel
        test_wasm_migrate_document
        test_wasm_fail_closed
        test_wasm_determinism)
else()
    set(_ctx_wasm_tests test_wasm_runner)
endif()

foreach(_t IN LISTS _ctx_wasm_tests)
    add_executable(context_wasm_${_t} tests/${_t}.cpp)
    target_link_libraries(context_wasm_${_t} PRIVATE context_wasm_runner context_warnings)
    target_include_directories(context_wasm_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # Self-contained GCC runtime (the setup.md libstdc++ DLL-shadowing quirk on MinGW hosts).
    target_link_options(context_wasm_${_t}
        PRIVATE $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
    if(_ctx_wasm_runtime)
        # The tests drive the REAL backend and assemble WAT guests via wasmtime_wat2wasm, so they
        # link context_wasm directly too.
        target_link_libraries(context_wasm_${_t} PRIVATE context_wasm)
        target_compile_definitions(context_wasm_${_t} PRIVATE CONTEXT_WASM_HAS_RUNTIME)
        if(WIN32)
            # The Windows link is against the wasmtime.dll IMPORT lib — stage the DLL next to the
            # test executable so ctest can launch it (the PR-1 comment's "consumer stages
            # wasmtime.dll next to itself").
            add_custom_command(TARGET context_wasm_${_t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_ctx_wasm_dest}/lib/wasmtime.dll"
                        "$<TARGET_FILE_DIR:context_wasm_${_t}>")
        endif()
    endif()
    add_test(NAME wasm-runner-${_t} COMMAND context_wasm_${_t})
endforeach()

# The determinism gate reads the COMMITTED fixture corpus off disk (byte-reproducible artifacts, not
# runtime-assembled WAT) — hand it the fixtures dir. Only defined in real mode (the target exists
# only there; the fixtures are executed by wasmtime).
if(_ctx_wasm_runtime)
    target_compile_definitions(context_wasm_test_wasm_determinism
        PRIVATE CONTEXT_WASM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
endif()
