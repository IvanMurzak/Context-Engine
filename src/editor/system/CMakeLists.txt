# src/editor/system/ — the (query, executor) system model over the kernel World + derived TS accessor
# codegen (M3 task 3b: R-LANG-009 / R-LANG-010 item 1 / R-LANG-002, issue #90; Part of #88). Builds
# directly on the now-landed runSystemView zero-copy view-lifetime gate (runtime/js, #89) and the
# declarative component storage (editor/component, #87): a system is a (query, executor) pair whose
# executor runs ONCE over zero-copy views of the queried declared components, and the derived TS
# accessors give authored TS typed get/set into those views. The keystone: an authored TS system
# mutates a declared World component, observable in the derived World.
#
# The library builds on EVERY toolchain (it only names the JsEngine INTERFACE + ViewBinding, no V8
# linkage); only the keystone in-V8 ctest links context_js + context_ts, so it is CI-only for its V8
# dependency path (local Strawberry-GCC dev gate -> js stub -> reduced assertion), mirroring
# runtime/ts's test_ts_in_v8. Depends on context_component (declarative types), context_kernel (World),
# and context_js (the runSystemView gate). OWN stanza in src/CMakeLists.txt — apart from the editor
# cluster + the exit-gate block — so concurrent sibling tasks never collide at the file tail (consumers
# name the `context_system` target, resolved at generate time regardless of add_subdirectory order).
# MUST follow add_subdirectory(runtime/js) and add_subdirectory(runtime/ts) so context_js / context_ts
# exist for the keystone test's link + the esbuild staging is available.

add_library(context_system STATIC
    src/accessor_codegen.cpp
    src/system.cpp)

target_include_directories(context_system PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: the public headers expose the declarative component types (component_registry.h /
# component_type.h), the kernel World/Entity, and the JS host's JsEngine/ViewBinding.
target_link_libraries(context_system PUBLIC context_component context_kernel context_js)
# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_system PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -------------------------------
# The repo carries no C++ test framework (each test is a plain assertion-bearing executable registered
# with ctest; tests/system_test.h is the tiny CHECK() harness). Two LOCAL gates (no V8): the derived
# accessor codegen (deterministic strings) and the engine-independent query/gather/scatter logic. One
# CI-only keystone: the full authored-TS-mutates-the-World in-V8 flow.

# 1) Derived TS accessor codegen — pure string derivation, a LOCAL gate on every leg.
add_executable(context_system_test_accessor_codegen tests/test_accessor_codegen.cpp)
target_link_libraries(context_system_test_accessor_codegen PRIVATE context_system context_warnings)
target_include_directories(context_system_test_accessor_codegen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME system-test_accessor_codegen COMMAND context_system_test_accessor_codegen)

# 2) Entity selection + gather/scatter round-trip — needs no JS host, a LOCAL gate on every leg.
add_executable(context_system_test_query tests/test_system_query.cpp)
target_link_libraries(context_system_test_query PRIVATE context_system context_warnings)
target_include_directories(context_system_test_query PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME system-test_query COMMAND context_system_test_query)

# 3) Keystone: authored TS mutates the shared World through zero-copy views. Links context_ts (esbuild
# transpile of the authored system + its generated accessor) + context_js (the V8 host). CI-only for
# its V8 path — mirror runtime/js's _ctx_js_v8 gate EXACTLY so the two agree leg-for-leg.

# --- resolve the esbuild platform + stage the pinned binary (verify-before-use, fail-closed) ------
# Self-contained (does not reach into runtime/ts's internal dest): the fetch is idempotent, so this is
# a no-op verify when runtime/ts already staged the same binary. Mirrors runtime/ts's block.
set(_ctx_sys_esbuild_platform "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_sys_esbuild_platform "win32-x64")
elseif(APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ctx_sys_esbuild_platform "darwin-arm64")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_sys_esbuild_platform "linux-x64")
else()
    message(FATAL_ERROR
        "context_system: no pinned esbuild prebuilt for host '${CMAKE_SYSTEM_NAME}/"
        "${CMAKE_SYSTEM_PROCESSOR}' — add a platform to tools/ts-toolchain.json (R-LANG-002).")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_ctx_sys_ts_manifest "${CMAKE_SOURCE_DIR}/../tools/ts-toolchain.json")
set(_ctx_sys_ts_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_esbuild.py")
set(_ctx_sys_ts_dest "${CMAKE_BINARY_DIR}/_esbuild/${_ctx_sys_esbuild_platform}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_ctx_sys_ts_fetch}"
            --manifest "${_ctx_sys_ts_manifest}"
            --platform "${_ctx_sys_esbuild_platform}"
            --dest "${_ctx_sys_ts_dest}"
    RESULT_VARIABLE _ctx_sys_ts_rc
    OUTPUT_VARIABLE _ctx_sys_ts_out
    ERROR_VARIABLE _ctx_sys_ts_err)
if(NOT _ctx_sys_ts_rc EQUAL 0)
    message(FATAL_ERROR
        "context_system: esbuild prebuilt fetch/verify FAILED (rc=${_ctx_sys_ts_rc}).\nstdout: "
        "${_ctx_sys_ts_out}\nstderr: ${_ctx_sys_ts_err}")
endif()
if(WIN32)
    set(_ctx_sys_esbuild_bin "${_ctx_sys_ts_dest}/bin/esbuild.exe")
else()
    set(_ctx_sys_esbuild_bin "${_ctx_sys_ts_dest}/bin/esbuild")
endif()

# --- resolve whether this toolchain links the rusty_v8 prebuilt (mirror runtime/js EXACTLY) -------
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
    set(_ctx_js_v8 OFF)  # honor runtime/js's stub escape hatch so the three stay in lockstep
endif()

# The staged esbuild binary path + the authored .ts examples dir are handed to the test as quoted
# compile-definition string literals (same shape as runtime/ts).
set(_ctx_sys_test_defs
    "CONTEXT_ESBUILD_PATH=\"${_ctx_sys_esbuild_bin}\""
    "CONTEXT_SYSTEM_EXAMPLES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/examples\"")

add_executable(context_system_test_in_v8 tests/test_system_in_v8.cpp)
target_link_libraries(context_system_test_in_v8
    PRIVATE context_system context_ts context_js context_warnings)
target_include_directories(context_system_test_in_v8 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_system_test_in_v8 PRIVATE ${_ctx_sys_test_defs})
if(_ctx_js_v8)
    target_compile_definitions(context_system_test_in_v8 PRIVATE CONTEXT_JS_HAS_V8)
endif()

# The test uses std::filesystem for temp-file plumbing; on the Strawberry-GCC Windows host a STALE
# PATH libstdc++-6.dll can shadow the std::filesystem symbols at ctest launch (Exit code 0xc0000139
# / STATUS_ENTRYPOINT_NOT_FOUND), so static-link the GCC runtime — gated to GNU so the MSVC/Clang CI
# legs are untouched (profile setup.md § the std::filesystem note).
target_link_options(context_system_test_in_v8 PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
add_test(NAME system-test_in_v8 COMMAND context_system_test_in_v8)

# TSan preset only: the in-V8 test links the rusty_v8 prebuilt (NOT TSan-instrumented), so reuse
# runtime/js's narrow suppressions file for the SAME benign V8-internal worker-pool races — scoped to
# this one test, guarded by CONTEXT_TSAN so ASan/UBSan + the normal dev build are untouched.
if(_ctx_js_v8 AND CONTEXT_TSAN)
    set_tests_properties(system-test_in_v8 PROPERTIES
        ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/js/tsan-suppressions.txt")
endif()

# Sanitize preset only (mutually exclusive with the tsan block): the in-V8 test links the
# uninstrumented rusty_v8 prebuilt, so ASan's LSan leak-check attributes the SANITIZER's OWN
# symbolizer/demangler scratch buffers (walked while symbolizing uninstrumented V8 frames) as leaks —
# never Context code. Reuse runtime/ts's narrow symbolizer-frame suppressions file, guarded by
# CONTEXT_SANITIZE + _ctx_js_v8 so the normal dev build + non-V8 toolchains are untouched.
if(_ctx_js_v8 AND CONTEXT_SANITIZE)
    set_tests_properties(system-test_in_v8 PROPERTIES
        ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ts/lsan-suppressions.txt")
endif()
