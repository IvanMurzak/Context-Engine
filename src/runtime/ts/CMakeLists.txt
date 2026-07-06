# src/runtime/ts/ — the M3 TypeScript toolchain (issue #83 / L-61 / R-LANG-002/004). The build
# half of the TS scripting tier: transpile + bundle authored .ts into a JS module the runtime/js
# V8 host (task 2a) evaluates. Backend = the SHA-pinned esbuild prebuilt (tools/ts-toolchain.json
# + tools/fetch_esbuild.py — TLS + SHA-pin + verify-before-use, fail-closed; the wgpu-native/CEF/
# rusty_v8 third-party-prebuilt precedent in docs/signing.md — NOT the first-party R-SEC-009 trust
# root, so NOT via tools/verify_artifact.py).
#
# esbuild is a build-TIME native binary invoked as a SUBPROCESS (never linked), so — unlike
# runtime/js's rusty_v8 link — context_ts builds + runs on EVERY toolchain including the local
# Ninja + Strawberry-GCC Windows dev gate. Its transpile ctest (test_ts_toolchain) is a LOCAL
# gate; the in-V8 integration ctest (test_ts_in_v8) links context_js and so is CI-only for its V8
# dependency path (local dev gate builds the js stub -> reduced assertion; see that file).

# --- resolve the esbuild platform key (OS + arch only; esbuild is compiler-agnostic) ----------
set(_ctx_esbuild_platform "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_esbuild_platform "win32-x64")
elseif(APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ctx_esbuild_platform "darwin-arm64")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_esbuild_platform "linux-x64")
else()
    # No pinned esbuild for this host — HALT the build (DoD: "if no suitable tool/prebuilt is
    # available for a CI leg -> HALT + report"). The three CI legs + the Windows dev host are all
    # covered; anything else needs a new pin added to tools/ts-toolchain.json first.
    message(FATAL_ERROR
        "context_ts: no pinned esbuild prebuilt for host '${CMAKE_SYSTEM_NAME}/"
        "${CMAKE_SYSTEM_PROCESSOR}' — add a platform to tools/ts-toolchain.json (R-LANG-002).")
endif()

# --- acquire + verify the pinned esbuild binary (verify-before-use, fail-closed) --------------
# FindPython3 resolves the real interpreter (incl. the `py` launcher on Windows), robust to the
# python3-vs-py alias difference on the self-hosted Windows runner — exactly like runtime/js.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_ctx_ts_manifest "${CMAKE_SOURCE_DIR}/../tools/ts-toolchain.json")
set(_ctx_ts_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_esbuild.py")
set(_ctx_ts_dest "${CMAKE_BINARY_DIR}/_esbuild/${_ctx_esbuild_platform}")
message(STATUS "context_ts: fetching + verifying pinned esbuild for ${_ctx_esbuild_platform}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_ctx_ts_fetch}"
            --manifest "${_ctx_ts_manifest}"
            --platform "${_ctx_esbuild_platform}"
            --dest "${_ctx_ts_dest}"
    RESULT_VARIABLE _ctx_ts_rc
    OUTPUT_VARIABLE _ctx_ts_out
    ERROR_VARIABLE _ctx_ts_err)
if(NOT _ctx_ts_rc EQUAL 0)
    message(FATAL_ERROR
        "context_ts: esbuild prebuilt fetch/verify FAILED (rc=${_ctx_ts_rc}) — refusing to build "
        "against an unverified esbuild (verify-before-use fail-closed).\nstdout: ${_ctx_ts_out}\n"
        "stderr: ${_ctx_ts_err}")
endif()
string(STRIP "${_ctx_ts_out}" _ctx_ts_out)
message(STATUS "context_ts: ${_ctx_ts_out}")

if(WIN32)
    set(_ctx_esbuild_bin "${_ctx_ts_dest}/bin/esbuild.exe")
else()
    set(_ctx_esbuild_bin "${_ctx_ts_dest}/bin/esbuild")
endif()

# --- the toolchain driver -----------------------------------------------------------------------
add_library(context_ts STATIC src/esbuild_toolchain.cpp)
target_include_directories(context_ts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_ts PRIVATE context_warnings)

# The driver + its tests use std::filesystem for temp-file plumbing. On the Strawberry-GCC
# Windows host a STALE PATH libstdc++-6.dll can shadow the std::filesystem symbols at ctest launch
# (Exit code 0xc0000139 / STATUS_ENTRYPOINT_NOT_FOUND), so static-link the GCC runtime — gated to
# GNU so the MSVC/Clang CI legs are untouched (profile setup.md § the std::filesystem note).
set(_ctx_ts_gcc_static "$<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>")

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# The staged esbuild binary path + the authored .ts examples dir are handed to the tests as
# compile definitions (quoted string literals).
set(_ctx_ts_test_defs
    "CONTEXT_ESBUILD_PATH=\"${_ctx_esbuild_bin}\""
    "CONTEXT_TS_EXAMPLES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/examples\"")

# 1) Transpile/bundle in isolation — a LOCAL gate (esbuild is a subprocess, links no V8).
add_executable(context_ts_test_ts_toolchain tests/test_ts_toolchain.cpp)
target_link_libraries(context_ts_test_ts_toolchain PRIVATE context_ts context_warnings)
target_compile_definitions(context_ts_test_ts_toolchain PRIVATE ${_ctx_ts_test_defs})
target_link_options(context_ts_test_ts_toolchain PRIVATE ${_ctx_ts_gcc_static})
add_test(NAME ts-test_ts_toolchain COMMAND context_ts_test_ts_toolchain)

# 2) Authored TS runs in the V8 host, host<->TS both ways — links context_js. CI-only for its V8
# path: mirror runtime/js's _ctx_js_v8 gate EXACTLY so the two agree leg-for-leg (real V8 only on
# Win-x64-MSVC / macOS-ARM64 / Linux-x64; the local Strawberry-GCC Windows dev gate builds the js
# stub and this test takes its reduced-assertion branch).
set(_ctx_js_v8 OFF)
if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_js_v8 ON)
elseif(APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ctx_js_v8 ON)
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_js_v8 ON)
endif()
if(CONTEXT_JS_FORCE_STUB)
    set(_ctx_js_v8 OFF)  # honor runtime/js's stub escape hatch so the two stay in lockstep
endif()

add_executable(context_ts_test_ts_in_v8 tests/test_ts_in_v8.cpp)
target_link_libraries(context_ts_test_ts_in_v8 PRIVATE context_ts context_js context_warnings)
target_compile_definitions(context_ts_test_ts_in_v8 PRIVATE ${_ctx_ts_test_defs})
if(_ctx_js_v8)
    target_compile_definitions(context_ts_test_ts_in_v8 PRIVATE CONTEXT_JS_HAS_V8)
endif()
target_link_options(context_ts_test_ts_in_v8 PRIVATE ${_ctx_ts_gcc_static})
add_test(NAME ts-test_ts_in_v8 COMMAND context_ts_test_ts_in_v8)

# TSan preset only: the in-V8 test links the rusty_v8 prebuilt (NOT TSan-instrumented), so reuse
# runtime/js's narrow suppressions file for the SAME benign V8-internal worker-pool races — scoped
# to this one test, guarded by CONTEXT_TSAN so ASan/UBSan + the normal dev build are untouched.
if(_ctx_js_v8 AND CONTEXT_TSAN)
    set_tests_properties(ts-test_ts_in_v8 PROPERTIES
        ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../js/tsan-suppressions.txt")
endif()
