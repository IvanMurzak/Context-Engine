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

# 2b) Deferred structural changes (#88 item 3 — the R-LANG-009 command-buffer clause): per-system
#     kernel::CommandBuffer recording + the World-taking run_tick's between-systems flush (batch
#     boundaries in ascending registration order; per-system on the TS lane). Links no V8, so it is a
#     LOCAL gate on every leg AND a full subject of the ASan/UBSan + TSan CI legs — the parallel-safety
#     gate extends to the deferred-structural path (16 systems recording into their own buffers across
#     a real fork-join).
add_executable(context_schedule_test_deferred tests/test_schedule_deferred.cpp)
target_link_libraries(context_schedule_test_deferred
    PRIVATE context_schedule context_kernel context_warnings Threads::Threads)
target_include_directories(context_schedule_test_deferred PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME schedule-test_deferred COMMAND context_schedule_test_deferred)

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

# MSVC + rusty_v8 std::exception_ptr symbol collision (this test target ONLY, when the V8 prebuilt is
# actually linked). schedule.cpp captures each helper thread's failure into a std::exception_ptr and
# rethrows the first on the join thread (the fork-join exception-propagation path, R-QA-013). On MSVC
# that TU emits COMDAT definitions of the std::exception_ptr family (ctor/copy/assign/dtor/operator
# bool, std::current_exception, std::rethrow_exception). The denoland/rusty_v8 prebuilt bundles its own
# custom libc++ STATICALLY (use_custom_libcxx=true — see runtime/js/CMakeLists.txt), whose exception.obj
# provides STRONG (non-COMDAT) definitions of the SAME MSVC-ABI-mangled std:: names, pulled in for V8's
# internal exception use. COMDAT-vs-strong of the same symbol → MSVC LNK2005 (multiply defined) → fatal
# LNK1169 when this exe links BOTH context_schedule.lib and rusty_v8.lib (via context_js). The sibling
# in-V8 keystone context_system_test_in_v8 links rusty_v8 identically WITHOUT this problem precisely
# because none of its TUs touch std::exception_ptr — so this is the one delta the exception fan-in adds.
#
# /FORCE:MULTIPLE keeps the FIRST definition of each duplicated symbol (context_schedule is first on the
# link line, so the MSVC-STL definitions win and rusty_v8's are dropped) and downgrades LNK2005 to a
# warning. This is SAFE, not a weakening: on Windows both MSVC-STL and rusty_v8's libc++ implement
# std::exception_ptr over the SAME underlying MSVC CRT __ExceptionPtr* primitives, so the chosen
# definitions are ABI-compatible for both sides. Scope is exactly this ONE keystone TEST exe on the
# MSVC leg — it does NOT touch the context_schedule library, product code, the non-V8 test_schedule
# (the authoritative parallel-safety subject, links no V8), or any sanitizer leg (ASan/UBSan + TSan run
# on ubuntu Clang/GCC, excluded by the MSVC generator expression). The scheduler's parallel-safety and
# exception-propagation correctness remain proven by test_schedule + the sanitizer gates. Guarded by
# _ctx_js_v8 so the CONTEXT_JS_FORCE_STUB / non-V8 toolchains (incl. the local Strawberry-GCC dev gate)
# never see it — there the stub context_js links no rusty_v8 and no collision exists.
if(_ctx_js_v8)
    target_link_options(context_schedule_test_in_v8 PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/FORCE:MULTIPLE>)
endif()

# Sanitize preset (ASan+UBSan, Linux/clang CI leg) UBSan `vptr` carve-out (issue #145; SAME false
# positive + SAME fix as runtime/ts's context_ts / context_ts_test_ts_debug, issue #104, and the sibling
# context_system_test_in_v8). This test TU links the UNINSTRUMENTED rusty_v8 prebuilt (via context_js),
# which bundles Chromium's OWN libc++/libc++abi statically (use_custom_libcxx, the `__Cr` ABI). Its RTTI
# in the same binary confuses UBSan's `vptr` sub-check into flagging this TU's own std stream member
# calls — the read_file helper's `in.rdbuf()` / `ss << in.rdbuf()` / `ss.str()`
# (test_schedule_in_v8.cpp:91-92) — as "member call on ... not of type std::basic_ostream", even though
# the object IS a valid libstdc++ std::__cxx11 stream and every functional assertion passes (the
# diagnostics rode green CI under UBSan default-recover). It is a FALSE POSITIVE of mixing an
# instrumented embedder with an uninstrumented prebuilt, NOT a Context-code defect (Context stays
# libstdc++, never libc++). Drop ONLY the vptr sub-check for THIS TU under the sanitize preset (ASan +
# every other UBSan check stay ON); this also trims the demangler-driven LSan symbolizer allocations
# those vptr diagnostics produced (see the LSan block below). Scoped to CONTEXT_SANITIZE so dev/CI
# release builds stay byte-identical, gated on _ctx_js_v8 (only the real-V8 legs link rusty_v8), and
# gated non-MSVC because -fno-sanitize=vptr is a Clang/GCC UBSan flag (MSVC has no vptr sanitizer, and
# CONTEXT_SANITIZE already FATAL_ERRORs under MSVC).
if(_ctx_js_v8 AND CONTEXT_SANITIZE AND NOT MSVC)
    target_compile_options(context_schedule_test_in_v8 PRIVATE -fno-sanitize=vptr)
endif()

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
