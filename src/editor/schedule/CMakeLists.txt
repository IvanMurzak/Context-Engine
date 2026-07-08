# src/editor/schedule/ — the safe parallel scheduler (M3 task 4a: R-SIM-006 / R-LANG-011 / L-38,
# issue #92; Part of #88). From the systems' STATIC declared component read/write sets it derives a
# parallel schedule DAG ONCE and CACHES it (rebuilt only on a composition change, never per frame):
# non-conflicting native/engine systems run in parallel batches; conflicting writes serialize; TS
# systems are EXCLUDED from the DAG and run on the single JS-VM lane (R-LANG-011). Builds on the
# (query, executor) system model (editor/system, #91) for the TS lane's run_system.
#
# The core library (context_schedule) orchestrates OPAQUE runnables and names only ComponentId (the
# AccessSet) + std::thread, so it builds on EVERY toolchain and its DAG/batching/parallel unit tests
# are a LOCAL gate on every leg — including, crucially, the ASan/UBSan + TSan CI legs, which fully
# instrument the real fork-join parallel execution (the authoritative parallel-safety gate; no V8
# suppressions needed there because this library links no V8). Only the C++-and-TS-both-mutate-World
# keystone links context_js/context_ts/context_system, so THAT test is CI-only for its V8 dependency
# path (mirrors editor/system's in-V8 keystone). OWN stanza in src/CMakeLists.txt — apart from the
# editor cluster + the exit-gate block — so concurrent sibling tasks never collide at the file tail
# (consumers name the `context_schedule` target, resolved at generate time regardless of order). MUST
# follow add_subdirectory(editor/system) so context_system exists for the keystone test's link.

add_library(context_schedule STATIC
    src/access.cpp
    src/schedule.cpp)

target_include_directories(context_schedule PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: access.h exposes kernel::ComponentId (context_kernel). PRIVATE: std::thread needs the
# Threads flag on the Linux/GCC leg (a no-op on the Windows/MSVC + MinGW native-thread legs).
target_link_libraries(context_schedule PUBLIC context_kernel)
target_link_libraries(context_schedule PRIVATE Threads::Threads)
# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_schedule PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -------------------------------
# The repo carries no C++ test framework (each test is a plain assertion-bearing executable registered
# with ctest; tests/schedule_test.h is the tiny CHECK() harness). Two LOCAL gates (no V8): the access
# conflict predicate, and the DAG construction + cache-invalidation + batching + REAL fork-join
# parallel execution over a World. One CI-only keystone: a C++ system and a TS system both mutate the
# shared World safely in the same tick.

# 1) Declared-access conflict predicate — pure logic, a LOCAL gate on every leg.
add_executable(context_schedule_test_access tests/test_access.cpp)
target_link_libraries(context_schedule_test_access PRIVATE context_schedule context_warnings)
target_include_directories(context_schedule_test_access PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME schedule-test_access COMMAND context_schedule_test_access)

# 2) DAG structure + cache invalidation + batching policy + real parallel execution over a World —
#    needs no JS host, so a LOCAL gate on every leg AND the full subject of the ASan/UBSan + TSan CI
#    legs (real std::thread fork-join over disjoint declared writes). Links context_kernel for World.
add_executable(context_schedule_test_schedule tests/test_schedule.cpp)
target_link_libraries(context_schedule_test_schedule
    PRIVATE context_schedule context_kernel context_component context_warnings Threads::Threads)
target_include_directories(context_schedule_test_schedule PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME schedule-test_schedule COMMAND context_schedule_test_schedule)

# 3) Keystone: a C++ (native) system and a TS system both mutate the shared World, scheduled safely in
#    one tick. The TS lane drives context_system's run_system in the V8 host; the native batch runs two
#    disjoint-write C++ systems in parallel. CI-only for its V8 path — mirror editor/system's
#    _ctx_js_v8 gate EXACTLY so the two agree leg-for-leg.

# --- resolve the esbuild platform + stage the pinned binary (verify-before-use, fail-closed) ------
# Self-contained (does not reach into runtime/ts's or editor/system's internal dest): the fetch is
# idempotent, so this is a no-op verify when a sibling already staged the same binary.
set(_ctx_sched_esbuild_platform "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_sched_esbuild_platform "win32-x64")
elseif(APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ctx_sched_esbuild_platform "darwin-arm64")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_ctx_sched_esbuild_platform "linux-x64")
else()
    message(FATAL_ERROR
        "context_schedule: no pinned esbuild prebuilt for host '${CMAKE_SYSTEM_NAME}/"
        "${CMAKE_SYSTEM_PROCESSOR}' — add a platform to tools/ts-toolchain.json (R-LANG-002).")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_ctx_sched_ts_manifest "${CMAKE_SOURCE_DIR}/../tools/ts-toolchain.json")
set(_ctx_sched_ts_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_esbuild.py")
set(_ctx_sched_ts_dest "${CMAKE_BINARY_DIR}/_esbuild/${_ctx_sched_esbuild_platform}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_ctx_sched_ts_fetch}"
            --manifest "${_ctx_sched_ts_manifest}"
            --platform "${_ctx_sched_esbuild_platform}"
            --dest "${_ctx_sched_ts_dest}"
    RESULT_VARIABLE _ctx_sched_ts_rc
    OUTPUT_VARIABLE _ctx_sched_ts_out
    ERROR_VARIABLE _ctx_sched_ts_err)
if(NOT _ctx_sched_ts_rc EQUAL 0)
    message(FATAL_ERROR
        "context_schedule: esbuild prebuilt fetch/verify FAILED (rc=${_ctx_sched_ts_rc}).\nstdout: "
        "${_ctx_sched_ts_out}\nstderr: ${_ctx_sched_ts_err}")
endif()
if(WIN32)
    set(_ctx_sched_esbuild_bin "${_ctx_sched_ts_dest}/bin/esbuild.exe")
else()
    set(_ctx_sched_esbuild_bin "${_ctx_sched_ts_dest}/bin/esbuild")
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
    set(_ctx_js_v8 OFF)  # honor runtime/js's stub escape hatch so the modules stay in lockstep
endif()

set(_ctx_sched_test_defs
    "CONTEXT_ESBUILD_PATH=\"${_ctx_sched_esbuild_bin}\""
    "CONTEXT_SCHEDULE_EXAMPLES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/examples\"")

add_executable(context_schedule_test_in_v8 tests/test_schedule_in_v8.cpp)
target_link_libraries(context_schedule_test_in_v8
    PRIVATE context_schedule context_system context_ts context_js context_component context_kernel
            context_warnings Threads::Threads)
target_include_directories(context_schedule_test_in_v8 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_schedule_test_in_v8 PRIVATE ${_ctx_sched_test_defs})
if(_ctx_js_v8)
    target_compile_definitions(context_schedule_test_in_v8 PRIVATE CONTEXT_JS_HAS_V8)
endif()

# std::filesystem temp-file plumbing on the Strawberry-GCC Windows host: static-link the GCC runtime so
# a STALE PATH libstdc++-6.dll cannot shadow the std::filesystem symbols at ctest launch (Exit code
# 0xc0000139); gated to GNU so the MSVC/Clang CI legs are untouched (profile setup.md § filesystem).
target_link_options(context_schedule_test_in_v8 PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
add_test(NAME schedule-test_in_v8 COMMAND context_schedule_test_in_v8)

# TSan preset only: the in-V8 test links the rusty_v8 prebuilt (NOT TSan-instrumented), so reuse
# runtime/js's narrow suppressions file for the SAME benign V8-internal worker-pool races — scoped to
# this one test, guarded by CONTEXT_TSAN so ASan/UBSan + the normal dev build are untouched. (The
# scheduler's OWN parallel execution is proven race-clean by test_schedule above, which links no V8 and
# needs no suppression.)
if(_ctx_js_v8 AND CONTEXT_TSAN)
    set_tests_properties(schedule-test_in_v8 PROPERTIES
        ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/js/tsan-suppressions.txt")
endif()

# Sanitize preset only (mutually exclusive with the tsan block): reuse runtime/ts's narrow
# symbolizer-frame LSan suppressions for the uninstrumented rusty_v8 prebuilt, guarded by
# CONTEXT_SANITIZE + _ctx_js_v8 so the normal dev build + non-V8 toolchains are untouched.
if(_ctx_js_v8 AND CONTEXT_SANITIZE)
    set_tests_properties(schedule-test_in_v8 PROPERTIES
        ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ts/lsan-suppressions.txt")
endif()
