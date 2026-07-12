# src/packages/animation/ — the animation package (M6 P3, R-SYS-002 / R-SYS-008, issue #180), with the
# determinism split (docs/physics-determinism-decision.md + R-SIM-001):
#   * SIM-PATH animation is integer/fixed-point ONLY (Q16 int64 — the simmath core from M6-F0b): the
#     anim-graph playback cursor + transition blend (Animator) drives the accumulated ROOT MOTION
#     (RootMotion), so an animation-active world folds into the L-54 hierarchical state hash
#     byte-identically on Linux-x64 / Win-x64 / macOS-ARM64 (animation_world.cpp / skeletal.cpp /
#     components.h). Deterministic skeletal pose SAMPLE + BLEND (R-SYS-002) live on this fixed-point path;
#   * the FULL per-bone POSE an entity displays is a PRESENTATION OBSERVER (cosmetic_pose.cpp) that reads
#     sim state read-only, holds its own float pose, and folds into NO hash — the R-SIM-001
#     downstream-observer rule. The one float TU (cosmetic_pose.cpp) never touches the sim path, so it
#     cannot taint the deterministic build.
# Clips are DCC-import-only (R-ASSET-001): a Rig is populated from imported data, no in-engine authoring.
#
# It is a PACKAGE, not kernel core (R-KERNEL-001 stays minimal / L-60 microkernel invariant): it composes
# on the kernel World like its siblings (physics3d, physics2d, particles, simmath) and the kernel gains
# NO dependency on it — nothing under src/kernel/ links context_animation.

add_library(context_animation STATIC
    src/skeletal.cpp
    src/animation_world.cpp
    src/cosmetic_pose.cpp)

target_include_directories(context_animation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel + context_simmath: the public headers expose the kernel World/Entity (the ECS
# the package composes on) and the simmath fixed-point value types (Skeleton / Clip / Pose / Rig) — so
# consumers need both include dirs. The dependency direction is the package layer composing on the
# kernel; the kernel never links back (L-60). Animation needs no broad-phase, so — unlike the physics
# packages — this package has NO context_spatial dependency.
target_link_libraries(context_animation PUBLIC context_kernel context_simmath)

# PRIVATE context_session: the M6-F0b sim-component registration seam
# (register_package_sim_component / sim_components()) the animator + root-motion components fold into the
# hierarchical state hash through — only animation_world.cpp names it.
target_link_libraries(context_animation PRIVATE context_session)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_animation PRIVATE context_warnings)

# The test executables link context_session (hash_world / sim_components in assertions); on this MinGW
# host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's, so they static-link the
# GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC untouched; the same self-contained-binary
# rationale as runtime/session and the physics/particles packages).
set(_context_animation_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/animation_test.h is the tiny CHECK() harness. Coverage:
#   - test_components: sim-component registration by stable name (fields, idempotency, pristine
#     builtin set untouched) + hash_world folding (identical-world equality, field sensitivity) + the
#     POD int64 layout law;
#   - test_skeletal: deterministic fixed-point pose SAMPLE (keyframe interpolation, bind fallback,
#     loop/clamp) + BLEND (weight endpoints + nlerp unit-length) + hierarchy composition — the
#     R-SYS-002 skeletal-blending core, reproduced bit-identically across two runs;
#   - test_graph: anim-graph EVALUATION (parameter-gated transitions, in-order first-match,
#     self-transition guard) + the transition cross-fade + root-motion accumulation + rig validation;
#   - test_cosmetic: the R-SIM-001 presentation-observer invariant — arbitrary cosmetic pose activity
#     leaves the sim state hash byte-identical, never writes sim state, and registers no sim component,
#     while doing real (non-vacuous) float work;
#   - test_errors: the fail-closed anim.* refusals (invalid rig / entity / duplicate / step, missing
#     component), the world left untouched on refusal, and the code strings pinned to the contract
#     catalog's anim.* block.
set(_context_animation_tests
    test_components
    test_skeletal
    test_graph
    test_cosmetic
    test_errors)

foreach(_t IN LISTS _context_animation_tests)
    add_executable(context_animation_${_t} tests/${_t}.cpp)
    target_link_libraries(context_animation_${_t}
        PRIVATE context_animation context_session context_warnings)
    target_link_options(context_animation_${_t} PRIVATE ${_context_animation_static_runtime})
    target_include_directories(context_animation_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME animation-${_t} COMMAND context_animation_${_t})
endforeach()

# --- the M6 P3 animation DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, issue #180) ---------------
# An animation-active scene (several animators over a shared rig — distinct anim-graph parameters that
# drive locomotion-state transitions + blended root motion) stepped N fixed ticks through the REAL
# context_animation package, its per-tick hierarchical state hash (hash_world over sim_components())
# folded and asserted against a cross-platform GOLDEN. Joins the `determinism-*` ctest family, so the
# blocking CI "Determinism gate" step (-R "^determinism-") runs it on all three build-matrix legs; its
# target is ALSO on the strict-FP `deterministic` job's hand-maintained --target list in
# .github/workflows/ci.yml. Integer-only sim state + fixed-endian hashing make the golden portable.
add_executable(context_animation_determinism_scene tests/determinism_animation_scene.cpp)
target_link_libraries(context_animation_determinism_scene
    PRIVATE context_animation context_session context_warnings)
target_link_options(context_animation_determinism_scene PRIVATE ${_context_animation_static_runtime})
target_include_directories(context_animation_determinism_scene
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME determinism-animation-scene COMMAND context_animation_determinism_scene)
