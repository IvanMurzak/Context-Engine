# src/testing/ — the M1 deterministic fault-injection harness (R-QA-010): a reusable
# context_testing support library that drives the injectable kernel + file-sync + bridge seams under
# a seeded, reproducible fault schedule, plus the scenario tests that assert the design's
# resume-or-diagnostic / convergence / gap-then-re-snapshot invariants (R-FILE-002/004, R-BRIDGE-008).
# The seams are M1 architecture (R-QA-010: not retrofittable); this library consumes them READ-ONLY
# via their public headers and adds no production code.

add_library(context_testing STATIC
    src/fault_scenarios.cpp)

target_include_directories(context_testing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The harness drives three merged seams: the kernel platform (ManualClock / InlineTaskRunner), the
# file-sync layer (MemoryFileStore + FakeWatcher + Reconciler + WriteQueue / IntentLog) and the
# bridge event stream (EventStream + bounded Subscriber). Those types appear in the scenario bodies,
# so all three are PUBLIC. context_warnings is the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_testing
    PUBLIC context_kernel context_filesync context_bridge
    PRIVATE context_warnings)

# Self-contained GCC runtime for the ctest executables (mirrors src/editor/bridge + src/cli): the
# file-sync + bridge code pulls std::filesystem symbols, and on a MinGW host a stale libstdc++-6.dll
# elsewhere on PATH can shadow the toolchain's, yielding STATUS_ENTRYPOINT_NOT_FOUND at launch.
# Static libgcc + libstdc++ keep the local ctest gate deterministic; gated to GNU so the macOS/Clang
# and Windows/MSVC CI legs (libc++ / the MSVC runtime) are untouched.
set(_context_testing_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- scenario tests (R-QA-013: the harness ships WITH its tests in the same PR) -----------------
# Plain assertion-bearing executables registered with ctest (the sibling src/editor/ + src/kernel/
# pattern); tests/testing_test.h is the tiny CHECK() harness. Each fault class is fuzzed across a
# committed seed sweep — deterministic, so a failing seed replays exactly and is committed as a
# regression case (R-QA-011).
set(_context_testing_tests
    test_seeded_rng
    test_crash_recovery
    test_watcher_faults
    test_slow_client
    test_seed_replay)

foreach(_t IN LISTS _context_testing_tests)
    add_executable(context_testing_${_t} tests/${_t}.cpp)
    target_link_libraries(context_testing_${_t} PRIVATE context_testing context_warnings)
    target_link_options(context_testing_${_t} PRIVATE ${_context_testing_static_runtime})
    target_include_directories(context_testing_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME testing-${_t} COMMAND context_testing_${_t})
endforeach()
