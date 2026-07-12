# src/packages/input/ — the input package (M6 P7, R-SYS-007 / L-45, issue #186). The authoring/mapping/
# routing FRONT-END for player input: it maps raw device events (keyboard / mouse / gamepad / touch /
# VR-controller) through stackable action maps + a layered UI-capture stack (L-45) into the mapped
# ACTION layer, with deterministic UI-vs-gameplay focus arbitration (R-SYS-007), plus runtime rebinding.
#
# THE SEAM (input_router.h): the headless session already owns the ONE sim-facing input sink — a
# world-singleton InputState the `input` system folds injected session::ActionActivation into each tick
# (src/runtime/session/). This package FEEDS that same sink: route() emits session::TickInputs /
# ActionActivation — the EXISTING types the sink consumes — so there is NO parallel sim-path input
# representation and replay/determinism keep resting on the single InputState. The package registers no
# sim component of its own (verified by tests/test_feed.cpp).
#
# It is a PACKAGE, not kernel core (L-60 microkernel invariant): it composes on the session/kernel
# surface like its siblings (animation, audio, physics*, particles) and the kernel gains NO dependency
# on it — nothing under src/kernel/ links context_input.

add_library(context_input STATIC
    src/input_router.cpp)

target_include_directories(context_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_session: UNLIKE the sim packages (which keep context_session a PRIVATE registration
# detail), input's public API is DEFINED in terms of the session input vocabulary — route() returns a
# session::TickInputs of session::ActionActivation, the exact types the sim InputState sink consumes.
# Exposing them is the whole point of the seam (feed the existing sink, do not fork), so consumers of
# input_router.h need the session include dir. context_session PUBLIC-links context_kernel, so the
# kernel World/Entity surface (the ECS the package composes on, L-60) comes transitively; the kernel
# never links back.
target_link_libraries(context_input PUBLIC context_session)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_input PRIVATE context_warnings)

# The test executables link context_session (Session / InputState / hash_world in assertions); on this
# MinGW host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's, so they static-link
# the GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC untouched; the same self-contained-binary
# rationale as runtime/session and the sibling packages).
set(_context_input_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/input_test.h is the tiny CHECK() harness. Coverage:
#   - test_action_map: install-by-unique-id, the fail-closed refusals (malformed/duplicate context),
#     stacking (push/pop, idempotent re-push), and runtime rebinding of a bound action to a new source;
#   - test_router: the R-SYS-007 UI-vs-gameplay FOCUS ARBITRATION — highest-priority binder wins, a
#     capturing UI context blocks gameplay input, a non-capturing overlay falls through, shared-source
#     priority, value/phase pass-through;
#   - test_feed: THE SEAM — routed device events land in the EXISTING sim InputState through the
#     ordinary Session injection sink, the package registers NO new sim component (no fork), and the
#     recorded input stream (== replay stream) carries the fed actions;
#   - test_errors: the fail-closed input.* refusals, the router left untouched on refusal, and the code
#     strings pinned to the contract catalog's input.* block.
set(_context_input_tests
    test_action_map
    test_router
    test_feed
    test_errors)

foreach(_t IN LISTS _context_input_tests)
    add_executable(context_input_${_t} tests/${_t}.cpp)
    target_link_libraries(context_input_${_t}
        PRIVATE context_input context_session context_warnings)
    target_link_options(context_input_${_t} PRIVATE ${_context_input_static_runtime})
    target_include_directories(context_input_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME input-${_t} COMMAND context_input_${_t})
endforeach()

# --- the M6 P7 input DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, issue #186) --------------------
# A fixed device-event schedule (with a mid-run modal UI-capture window) routed through the REAL
# context_input package and fed into a real Session; each tick's hierarchical state hash (hash_world
# over the single sim_components() registry the package feeds — no fork) folded and asserted against a
# cross-platform GOLDEN. Joins the `determinism-*` ctest family, so the blocking CI "Determinism gate"
# step (-R "^determinism-") runs it on all three build-matrix legs; its target is ALSO on the strict-FP
# `deterministic` job's hand-maintained --target list in .github/workflows/ci.yml. Pure integer routing
# + integer InputState + fixed-endian hashing make the golden portable.
add_executable(context_input_determinism_scene tests/determinism_input_scene.cpp)
target_link_libraries(context_input_determinism_scene
    PRIVATE context_input context_session context_warnings)
target_link_options(context_input_determinism_scene PRIVATE ${_context_input_static_runtime})
target_include_directories(context_input_determinism_scene
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME determinism-input-scene COMMAND context_input_determinism_scene)
