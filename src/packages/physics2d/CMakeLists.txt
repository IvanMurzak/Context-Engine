# src/packages/physics2d/ — the deterministic fixed-point rigid-body 2D physics package (M6 P2,
# R-2D-002 / L-55, issue #176), the 2D sibling of packages/physics3d, implementing the F0a
# physics-core decision (docs/physics-determinism-decision.md): sim state is integer/fixed-point ONLY
# (Q16 int64 — the simmath core from M6-F0b), so a physics-active world folds into the L-54
# hierarchical state hash byte-identically on Linux-x64 / Win-x64 / macOS-ARM64. Box2D-class 2D
# rigid-body under the same R-KERNEL-004 package contract as 3D; broad-phase collision reuses
# context_spatial (a conservative degenerate-Z prune — the exact fixed-point narrow phase decides sim
# state; see physics_world.h).
#
# It is a PACKAGE, not kernel core (R-KERNEL-001 stays minimal / L-60 microkernel invariant): it
# composes on the kernel World like its siblings (physics3d, spatial, simmath) and the kernel gains NO
# dependency on it — nothing under src/kernel/ links context_physics2d.

add_library(context_physics2d STATIC
    src/physics_world.cpp)

target_include_directories(context_physics2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel + context_simmath: the public headers expose the kernel World/Entity (the
# ECS the package composes on) and the simmath fixed-point value types (BodyDesc / PhysicsConfig /
# BodyState) — consumers need both include dirs. The dependency direction is the package layer
# composing on the kernel; the kernel never links back (L-60).
target_link_libraries(context_physics2d PUBLIC context_kernel context_simmath)

# PRIVATE context_spatial: the broad-phase index is an implementation detail behind the pimpl (no
# spatial type appears in a public physics2d header). PRIVATE context_session: the M6-F0b
# sim-component registration seam (register_package_sim_component / sim_components()) physics
# components fold into the hierarchical state hash through — only physics_world.cpp names it.
target_link_libraries(context_physics2d PRIVATE context_spatial context_session)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_physics2d PRIVATE context_warnings)

# The test executables link context_session (hash_world / sim_components in assertions); on this
# MinGW host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's, so they
# static-link the GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC untouched; the same
# self-contained-binary rationale as physics3d, runtime/session, and src/cli).
set(_context_physics2d_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/physics2d_test.h is the tiny CHECK() harness. Coverage:
#   - test_components: sim-component registration by stable name (fields, idempotency, pristine
#     builtin set untouched) + hash_world folding (signature, field sensitivity, identical-world
#     equality) + the POD int64 layout law;
#   - test_dynamics: fixed-point integration (exact free-fall velocity), restitution bounce +
#     resting contact on a static floor, circle-circle impulse exchange + momentum, ramp slide
#     (circle-vs-rotated-box contact), static bodies never move, far-apart bodies never interact,
#     within-run determinism (two identical scenes hash identically after N steps);
#   - test_boxbox: the fixed-point oriented-box SAT + reference-face clip (issue #199) — a dynamic
#     box rests on a static box floor (no tunnel/tip via the two-point manifold), overlapping boxes
#     resolve apart, a box is pushable, a two-box stack settles + stays stacked, and the box-box
#     path is deterministic (two identical scenes hash identically after N steps);
#   - test_errors: the fail-closed physics2d.* refusals (invalid shape / mass / entity / step,
#     missing component set), the world left untouched on refusal, static-body no-op impulse, and
#     the code strings pinned to the contract catalog's physics2d.* block.
set(_context_physics2d_tests
    test_components
    test_dynamics
    test_boxbox
    test_errors)

foreach(_t IN LISTS _context_physics2d_tests)
    add_executable(context_physics2d_${_t} tests/${_t}.cpp)
    target_link_libraries(context_physics2d_${_t}
        PRIVATE context_physics2d context_session context_warnings)
    target_link_options(context_physics2d_${_t} PRIVATE ${_context_physics2d_static_runtime})
    target_include_directories(context_physics2d_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME physics2d-${_t} COMMAND context_physics2d_${_t})
endforeach()

# --- the M6 P2 physics-active DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, issue #176) ---------
# The 2D sibling of determinism-physics3d-scene: a 2D physics-active scene (dynamic circles + a
# pushable box on static box platforms + a tilted ramp) stepped N fixed ticks through the REAL
# context_physics2d package, its per-tick hierarchical state hash (hash_world over sim_components())
# folded and asserted against a cross-platform GOLDEN. Joins the `determinism-*` ctest family, so the
# blocking CI "Determinism gate" step (-R "^determinism-") runs it on all three build-matrix legs —
# ubuntu-latest (Linux-x64), windows-latest (Win-x64), macos-latest (macOS-ARM64) — with no new CI
# job. It is ALSO added to the strict-FP `deterministic` job's EXPLICIT --target list in
# .github/workflows/ci.yml (that job builds only a hand-maintained target list, unlike the `build`
# job's regex, so an unregistered determinism-* ctest would report "Not Run" → RED). Integer-only sim
# state + fixed-endian hashing make the golden portable.
add_executable(context_physics2d_determinism_scene tests/determinism_physics_scene.cpp)
target_link_libraries(context_physics2d_determinism_scene
    PRIVATE context_physics2d context_session context_warnings)
target_link_options(context_physics2d_determinism_scene PRIVATE ${_context_physics2d_static_runtime})
target_include_directories(context_physics2d_determinism_scene
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME determinism-physics2d-scene COMMAND context_physics2d_determinism_scene)
