# src/runtime/session/ — the headless simulation session + versioned replay artifact (M3 entry,
# R-QA-005 / L-54, issue #74). The deterministic headless harness the whole M3 automated-QA /
# determinism story rests on: fixed-tick session control (step-N / seed / synthetic input + action
# injection / record + replay / state-hash query), the monotonic simTick counter, the hierarchical
# canonical state hash (root + per-system + per-archetype + trace mode), and the versioned, schema'd
# replay-artifact kind (manifest-verified before run; first-divergence reported; non-deterministic
# replay labeled best-effort). Built on the kernel World/ECS + fixed-timestep Scheduler (R-SIM-002)
# and the canonical serializer (R-FILE-001 / L-32).

add_library(context_session STATIC
    src/sim_component.cpp
    src/input.cpp
    src/state_hash.cpp
    src/session.cpp
    src/session_state.cpp
    src/replay.cpp
    src/triage.cpp)

target_include_directories(context_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel: the public headers expose the kernel World/Entity/Component (the session's
# ECS state). PUBLIC context_serializer: the input/state-hash/replay headers expose the serializer
# JsonValue tree (session-state + replay docs are canonical engine files, R-FILE-001).
target_link_libraries(context_session PUBLIC context_kernel context_serializer)

# PRIVATE context_filesync: replay.cpp jails artifact-supplied manifest paths against the project
# root (R-SEC-008 is_inside_jail) before reading them — a ctx:replay artifact is untrusted, shareable
# input. Only the .cpp uses it (no filesync type appears in a public session header).
target_link_libraries(context_session PRIVATE context_filesync)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_session PRIVATE context_warnings)

# The engine version stamped into a recorded replay artifact (engineVersion field) — sourced from
# PROJECT_VERSION so it tracks the real engine release, never a hand-copied literal.
target_compile_definitions(context_session PRIVATE CONTEXT_ENGINE_VERSION="${PROJECT_VERSION}")

# The replay manifest verifier reads project input files via std::filesystem; on this MinGW host a
# stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's and be loaded first, so the
# session test executables static-link the GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC
# are untouched; see src/cli/CMakeLists.txt for the same self-contained-binary rationale).
set(_context_session_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/session_test.h is the tiny CHECK() harness.
set(_context_session_tests
    test_sim_component
    test_state_hash
    test_session
    test_session_state
    test_replay
    test_triage)

foreach(_t IN LISTS _context_session_tests)
    add_executable(context_session_${_t} tests/${_t}.cpp)
    target_link_libraries(context_session_${_t} PRIVATE context_session context_warnings)
    target_link_options(context_session_${_t} PRIVATE ${_context_session_static_runtime})
    target_include_directories(context_session_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME session-${_t} COMMAND context_session_${_t})
endforeach()

# --- the R-QA-005 / L-54 DETERMINISM GATE (blocking, 3-OS: Linux-x64 / Win-x64 / macOS-ARM64) ----
# A cross-platform GOLDEN-hash assertion: the demo scenario, stepped from a fixed seed + a fixed
# injected input stream, must produce a byte-identical hierarchical state-hash trace on every
# platform in the determinism matrix. Registered as the `determinism-*` ctest family the CI
# "Determinism gate" step runs on all three build-matrix legs (docs/ci-fleet-manifest.json:
# determinism-state-hash). Integer-only state + fixed-endian hashing make the golden values portable.
add_executable(context_session_determinism_gate tests/determinism_gate.cpp)
target_link_libraries(context_session_determinism_gate PRIVATE context_session context_warnings)
target_link_options(context_session_determinism_gate PRIVATE ${_context_session_static_runtime})
target_include_directories(context_session_determinism_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME determinism-state-hash COMMAND context_session_determinism_gate)

# M6-F0a physics-determinism SPIKE PROOF (issue #170): a trivial fixed-point physics-shaped step
# (integration + fixed-point drag + integer-restitution collisions, all integer arithmetic) folded
# through the same big-endian FNV-1a primitive, asserted against a cross-platform GOLDEN hash. Joins
# the `determinism-*` family so the blocking CI "Determinism gate" step runs it on all three legs —
# the evidence behind the fixed-point physics-core decision (docs/physics-determinism-decision.md).
# Only hash.h is used (header-only), but it links context_session for the shared test wiring pattern.
add_executable(context_session_determinism_physics tests/determinism_physics_gate.cpp)
target_link_libraries(context_session_determinism_physics PRIVATE context_session context_warnings)
target_link_options(context_session_determinism_physics PRIVATE ${_context_session_static_runtime})
target_include_directories(context_session_determinism_physics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME determinism-physics-wedge COMMAND context_session_determinism_physics)
