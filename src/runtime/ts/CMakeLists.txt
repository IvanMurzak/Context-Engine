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

# --- resolve the tsgo platform key (OS + arch only; tsgo is compiler-agnostic) ------------------
# The tsc-class SEMANTIC typechecker for the author->typecheck->fix loop (issue #85). esbuild strips
# types without checking them, so a --noEmit typecheck needs a SEPARATE tsc-class tool. tsgo
# (microsoft/typescript-go) is a native Go binary invoked as a SUBPROCESS (never linked), so — like
# esbuild — it builds/runs on EVERY toolchain including this Strawberry-GCC Windows dev gate, and its
# typecheck ctest is a LOCAL gate. Same OS+arch platform keys as esbuild (compiler-agnostic).
set(_ctx_tsgo_platform "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_tsgo_platform "win32-x64")
elseif(APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ctx_tsgo_platform "darwin-arm64")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_tsgo_platform "linux-x64")
else()
    message(FATAL_ERROR
        "context_ts: no pinned tsgo prebuilt for host '${CMAKE_SYSTEM_NAME}/"
        "${CMAKE_SYSTEM_PROCESSOR}' — add a platform to tools/tsc-toolchain.json (R-LANG-002).")
endif()

# --- acquire + verify the pinned tsgo binary + its lib dir (verify-before-use, fail-closed) -----
# fetch_tsc.py stages the WHOLE package/lib/ dir (the binary next to its lib.*.d.ts) — tsgo resolves
# its default libs from its own exe dir and PANICS if they are missing. Python3 was already resolved
# for the esbuild fetch above.
set(_ctx_tsc_manifest "${CMAKE_SOURCE_DIR}/../tools/tsc-toolchain.json")
set(_ctx_tsc_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_tsc.py")
set(_ctx_tsc_dest "${CMAKE_BINARY_DIR}/_tsc/${_ctx_tsgo_platform}")
message(STATUS "context_ts: fetching + verifying pinned tsgo for ${_ctx_tsgo_platform}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_ctx_tsc_fetch}"
            --manifest "${_ctx_tsc_manifest}"
            --platform "${_ctx_tsgo_platform}"
            --dest "${_ctx_tsc_dest}"
    RESULT_VARIABLE _ctx_tsc_rc
    OUTPUT_VARIABLE _ctx_tsc_out
    ERROR_VARIABLE _ctx_tsc_err)
if(NOT _ctx_tsc_rc EQUAL 0)
    message(FATAL_ERROR
        "context_ts: tsgo prebuilt fetch/verify FAILED (rc=${_ctx_tsc_rc}) — refusing to build "
        "against an unverified tsgo (verify-before-use fail-closed).\nstdout: ${_ctx_tsc_out}\n"
        "stderr: ${_ctx_tsc_err}")
endif()
string(STRIP "${_ctx_tsc_out}" _ctx_tsc_out)
message(STATUS "context_ts: ${_ctx_tsc_out}")

if(WIN32)
    set(_ctx_tsgo_bin "${_ctx_tsc_dest}/tsgo.exe")
else()
    set(_ctx_tsgo_bin "${_ctx_tsc_dest}/tsgo")
endif()

# --- the toolchain driver -----------------------------------------------------------------------
# esbuild_toolchain.cpp = the transpile/bundle + sourcemap-emission driver (subprocess); source_map
# .cpp + stack_trace.cpp = the STL-only, V8-free R-OBS-005 half (parse a Source Map v3, remap a raw
# V8 stack to authored .ts positions). All three build on EVERY toolchain (no V8 link), so their
# ctests are LOCAL gates.
add_library(context_ts STATIC
    src/esbuild_toolchain.cpp
    src/tsgo_typecheck.cpp
    src/source_map.cpp
    src/stack_trace.cpp)
target_include_directories(context_ts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_ts PRIVATE context_warnings)

# Sanitize preset (ASan+UBSan, Linux/clang CI leg) carve-out. ts-test_ts_in_v8 links the
# UNINSTRUMENTED rusty_v8 prebuilt (via context_js), which bundles Chromium's OWN libc++/libc++abi
# (use_custom_libcxx, the `__Cr` ABI). Its RTTI in the same binary confuses UBSan's `vptr` sub-check
# into flagging esbuild_toolchain.cpp's std::ostringstream / std::ifstream member calls as
# "member call on ... not of type std::basic_ostream" — even though the object IS a valid libstdc++
# std::__cxx11 stream (the UBSan note itself reports `vptr for std::__cxx11::basic_ostringstream`,
# and the test's functional assertions pass). It is a FALSE POSITIVE of mixing an instrumented
# embedder with an uninstrumented prebuilt, NOT a defect in Context code (which stays libstdc++,
# never libc++ — see runtime/js/tsan-suppressions.txt). Drop ONLY the vptr sub-check for THIS TU
# under the sanitize preset (ASan + every other UBSan check stay ON); this also removes the
# demangler-driven LSan symbolizer allocations those vptr diagnostics produced (see the
# lsan-suppressions.txt note below). Scoped to CONTEXT_SANITIZE so the dev/CI release builds are
# byte-identical, and gated non-MSVC because -fno-sanitize=vptr is a Clang/GCC UBSan flag (MSVC has
# no vptr sanitizer, and CONTEXT_SANITIZE already FATAL_ERRORs under MSVC).
if(CONTEXT_SANITIZE AND NOT MSVC)
    target_compile_options(context_ts PRIVATE -fno-sanitize=vptr)
endif()

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

# 1) Transpile/bundle + sourcemap emission in isolation — a LOCAL gate (esbuild is a subprocess,
# links no V8). Now also round-trips esbuild's REAL Source Map v3 through source_map.h.
add_executable(context_ts_test_ts_toolchain tests/test_ts_toolchain.cpp)
target_link_libraries(context_ts_test_ts_toolchain PRIVATE context_ts context_warnings)
target_compile_definitions(context_ts_test_ts_toolchain PRIVATE ${_ctx_ts_test_defs})
target_link_options(context_ts_test_ts_toolchain PRIVATE ${_ctx_ts_gcc_static})
add_test(NAME ts-test_ts_toolchain COMMAND context_ts_test_ts_toolchain)

# 1d) SEMANTIC typecheck via tsgo (--noEmit) — a LOCAL gate (tsgo is a subprocess, links no V8). It
# needs the staged TSGO path plus esbuild + the examples dir (the duality test transpiles a
# type-invalid file with esbuild and shows the typecheck rejects it). The author->typecheck->fix loop.
add_executable(context_ts_test_ts_typecheck tests/test_ts_typecheck.cpp)
target_link_libraries(context_ts_test_ts_typecheck PRIVATE context_ts context_warnings)
target_compile_definitions(context_ts_test_ts_typecheck PRIVATE
    ${_ctx_ts_test_defs}
    "CONTEXT_TSGO_PATH=\"${_ctx_tsgo_bin}\"")
target_link_options(context_ts_test_ts_typecheck PRIVATE ${_ctx_ts_gcc_static})
add_test(NAME ts-test_ts_typecheck COMMAND context_ts_test_ts_typecheck)

# 1a) Source Map v3 parser + resolver — a LOCAL gate (no V8, no esbuild needed at runtime; the
# hand-encoded VLQ maps are inline). The R-OBS-005 mapping foundation.
add_executable(context_ts_test_source_map tests/test_source_map.cpp)
target_link_libraries(context_ts_test_source_map PRIVATE context_ts context_warnings)
target_link_options(context_ts_test_source_map PRIVATE ${_ctx_ts_gcc_static})
add_test(NAME ts-test_source_map COMMAND context_ts_test_source_map)

# 1b) TS-resolved stack-trace remapper — a LOCAL gate (parses a canned V8 stack + remaps through a
# known map; no V8 link). The "headless CI still needs symbolicated traces" half of R-OBS-005.
add_executable(context_ts_test_stack_trace tests/test_stack_trace.cpp)
target_link_libraries(context_ts_test_stack_trace PRIVATE context_ts context_warnings)
target_link_options(context_ts_test_stack_trace PRIVATE ${_ctx_ts_gcc_static})
add_test(NAME ts-test_stack_trace COMMAND context_ts_test_stack_trace)

# 1c) R-SEC-005 end-to-end (issue #100): an engine-INSTALLED pure-JS package resolves + imports in
# the TS host. Installs a fixture package via context_editor_pkg (the R-SEC-005 install path) then
# bundles an authored .ts entry importing it through esbuild — proving node module-resolution finds
# the installed package. A LOCAL gate (esbuild is a subprocess; no V8 link). Links context_editor_pkg
# (the install path) alongside context_ts; needs the staged esbuild path define. NB: the target is
# NOT named *install* — a Windows UAC installer-detection trap (see editor/pkg/CMakeLists.txt).
add_executable(context_ts_test_pkg_resolve tests/test_pkg_resolve.cpp)
target_link_libraries(context_ts_test_pkg_resolve
    PRIVATE context_ts context_editor_pkg context_warnings)
target_compile_definitions(context_ts_test_pkg_resolve PRIVATE ${_ctx_ts_test_defs})
target_link_options(context_ts_test_pkg_resolve PRIVATE ${_ctx_ts_gcc_static})
add_test(NAME ts-test_pkg_resolve COMMAND context_ts_test_pkg_resolve)

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

# 3) R-OBS-005 INTERACTIVE half (issue #94): attach the V8 in-box CDP inspector, set a source-mapped
# breakpoint in authored TS (resolved to generated JS via the 2b-i map), hit it, resume. Links
# context_js (CI-only for its V8 path — same _ctx_js_v8 gate as test_ts_in_v8, so the two agree
# leg-for-leg); on the local stub gate it exercises the source-map half + asserts V8 unavailable.
add_executable(context_ts_test_ts_debug tests/test_ts_debug.cpp)
target_link_libraries(context_ts_test_ts_debug PRIVATE context_ts context_js context_warnings)
target_compile_definitions(context_ts_test_ts_debug PRIVATE ${_ctx_ts_test_defs})
if(_ctx_js_v8)
    target_compile_definitions(context_ts_test_ts_debug PRIVATE CONTEXT_JS_HAS_V8)
endif()
target_link_options(context_ts_test_ts_debug PRIVATE ${_ctx_ts_gcc_static})
# Same UBSan `vptr` false-positive carve-out as context_ts above (issue #104): this test TU links the
# UNINSTRUMENTED rusty_v8 prebuilt (via context_js) — its bundled Chromium libc++/libc++abi RTTI in
# the same binary confuses UBSan's `vptr` sub-check into flagging this TU's own std::ifstream / std
# stream member calls as "member call on ... not of type ...", even though the object IS a valid
# libstdc++ stream (a false positive of mixing an instrumented embedder with an uninstrumented
# prebuilt, NOT a Context-code defect). Drop ONLY the vptr sub-check for THIS TU under the sanitize
# preset (ASan + every other UBSan check stay ON); scoped to CONTEXT_SANITIZE so dev/CI release builds
# are byte-identical, and gated non-MSVC because -fno-sanitize=vptr is a Clang/GCC UBSan flag.
if(_ctx_js_v8 AND CONTEXT_SANITIZE AND NOT MSVC)
    target_compile_options(context_ts_test_ts_debug PRIVATE -fno-sanitize=vptr)
endif()
add_test(NAME ts-test_ts_debug COMMAND context_ts_test_ts_debug)

# Same benign-V8-worker-pool suppressions as ts-test_ts_in_v8 (it links the SAME uninstrumented
# rusty_v8 prebuilt), scoped to this one test under each sanitizer preset.
if(_ctx_js_v8 AND CONTEXT_TSAN)
    set_tests_properties(ts-test_ts_debug PROPERTIES
        ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../js/tsan-suppressions.txt")
endif()
if(_ctx_js_v8 AND CONTEXT_SANITIZE)
    set_tests_properties(ts-test_ts_debug PROPERTIES
        ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan-suppressions.txt")
endif()

# Sanitize preset only (mutually exclusive with the tsan block above — the two presets never
# co-configure, so their ENVIRONMENT sets never clash): the in-V8 test links the uninstrumented
# rusty_v8 prebuilt, so ASan's LSan leak-check attributes the SANITIZER's OWN symbolizer/demangler
# scratch buffers (libc++abi ItaniumDemangle via __cxa_demangle, walked while symbolizing
# uninstrumented V8 frames whose stack terminates in `0xaaaaaaaa... (<unknown module>)`) as leaks —
# never Context (`namespace context::`) code. The -fno-sanitize=vptr carve-out on context_ts above
# removes the vptr diagnostics that drove most of that demangling; this narrow suppressions file is
# a safety net matching ONLY the sanitizer's symbolizer frames (so it can never mask a real
# context:: leak), guarded by CONTEXT_SANITIZE + _ctx_js_v8 so the normal dev build + the non-V8
# toolchains are untouched.
if(_ctx_js_v8 AND CONTEXT_SANITIZE)
    set_tests_properties(ts-test_ts_in_v8 PROPERTIES
        ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan-suppressions.txt")
endif()
