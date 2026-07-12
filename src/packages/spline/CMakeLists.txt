# src/packages/spline/ — the spline package (M6 P5, R-SYS-004 SHOULD), with the determinism split
# (docs/physics-determinism-decision.md + R-SIM-001):
#   * SIM-PATH spline-driven movement is integer/fixed-point ONLY (Q16 int64 — the simmath core from
#     M6-F0b): a PathFollower advances by arc length along a deterministic curve (de Casteljau /
#     Catmull-Rom fixed cubics, arc-length via fixed_sqrt, heading via the deterministic fixed_atan2),
#     so a spline-active world folds into the L-54 hierarchical state hash byte-identically on
#     Linux-x64 / Win-x64 / macOS-ARM64 (curve.cpp / spline_world.cpp / components.h). No float, no
#     platform libm on the sim path;
#   * the tooling/geometry curve DISPLAY (tessellated float polylines + observed markers) is a
#     PRESENTATION OBSERVER (cosmetic_curve.cpp) that reads sim state read-only, holds its own float
#     display geometry, and folds into NO hash — the R-SIM-001 downstream-observer rule. The one float
#     TU (cosmetic_curve.cpp) never touches the sim path, so it cannot taint the deterministic build.
#
# It is a PACKAGE, not kernel core (R-KERNEL-001 stays minimal / L-60 microkernel invariant): it
# composes on the kernel World like its siblings (physics3d, physics2d, particles, animation, simmath)
# and the kernel gains NO dependency on it — nothing under src/kernel/ links context_spline. Splines do
# not collide, so — unlike the physics packages — this package has NO context_spatial dependency.

add_library(context_spline STATIC
    src/curve.cpp
    src/spline_world.cpp
    src/cosmetic_curve.cpp)

target_include_directories(context_spline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel + context_simmath: the public headers expose the kernel World/Entity (the ECS
# the package composes on) and the simmath fixed-point value types (Curve / PathFollower positions) — so
# consumers need both include dirs. The dependency direction is the package layer composing on the
# kernel; the kernel never links back (L-60).
target_link_libraries(context_spline PUBLIC context_kernel context_simmath)

# PRIVATE context_session: the M6-F0b sim-component registration seam
# (register_package_sim_component / sim_components()) the path-follower component folds into the
# hierarchical state hash through — only spline_world.cpp names it.
target_link_libraries(context_spline PRIVATE context_session)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_spline PRIVATE context_warnings)

# The test executables link context_session (hash_world / sim_components in assertions); on this MinGW
# host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's, so they static-link the
# GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC untouched; the same self-contained-binary
# rationale as runtime/session and the physics/particles/animation packages).
set(_context_spline_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/spline_test.h is the tiny CHECK() harness. Coverage:
#   - test_components: sim-component registration by stable name (fields, idempotency, pristine builtin
#     set untouched) + hash_world folding (identical-world equality, field sensitivity) + the POD int64
#     layout law;
#   - test_curve: deterministic fixed-point curve evaluation — validity/segment counting, Bezier end +
#     Catmull-Rom interior interpolation (exact at the joints), unit-length tangent, positive/chord-
#     bounded arc length, bit-for-bit reproducibility;
#   - test_follower: the path-follow sim — arc-length advance, non-loop clamp at both ends, loop wrap,
#     set_speed, attach/detach;
#   - test_cosmetic: the R-SIM-001 presentation-observer invariant — arbitrary display activity leaves
#     the sim state hash byte-identical, never writes sim state, and registers no sim component, while
#     doing real (non-vacuous) float tessellation + shimmer work;
#   - test_errors: the fail-closed spline.* refusals (invalid path set / index, missing component,
#     duplicate, non-positive step), the world left untouched on refusal, and the code strings pinned to
#     the contract catalog's spline.* block.
set(_context_spline_tests
    test_components
    test_curve
    test_follower
    test_cosmetic
    test_errors)

foreach(_t IN LISTS _context_spline_tests)
    add_executable(context_spline_${_t} tests/${_t}.cpp)
    target_link_libraries(context_spline_${_t}
        PRIVATE context_spline context_session context_warnings)
    target_link_options(context_spline_${_t} PRIVATE ${_context_spline_static_runtime})
    target_include_directories(context_spline_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME spline-${_t} COMMAND context_spline_${_t})
endforeach()

# --- the M6 P5 spline DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, R-SYS-004) -------------------
# A spline-active scene (several followers advancing along a shared multi-path set — distinct speeds,
# a mix of loop/clamp, a fixed mid-run speed schedule) stepped N fixed ticks through the REAL
# context_spline package, its per-tick hierarchical state hash (hash_world over sim_components()) folded
# and asserted against a cross-platform GOLDEN. Joins the `determinism-*` ctest family, so the blocking
# CI "Determinism gate" step (-R "^determinism-") runs it on all three build-matrix legs; its target is
# ALSO on the strict-FP `deterministic` job's hand-maintained --target list in .github/workflows/ci.yml.
# Integer-only sim state + fixed-endian hashing make the golden portable.
add_executable(context_spline_determinism_scene tests/determinism_spline_scene.cpp)
target_link_libraries(context_spline_determinism_scene
    PRIVATE context_spline context_session context_warnings)
target_link_options(context_spline_determinism_scene PRIVATE ${_context_spline_static_runtime})
target_include_directories(context_spline_determinism_scene
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME determinism-spline-scene COMMAND context_spline_determinism_scene)
