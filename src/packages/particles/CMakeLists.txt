# src/packages/particles/ — the particle-system package (M6 P4, R-SYS-003, issue #178), with the
# CORRECT determinism split (docs/physics-determinism-decision.md + R-SIM-001):
#   * SIM-PATH particles are integer/fixed-point ONLY (Q16 int64 — the simmath core from M6-F0b), so a
#     particle-active world folds into the L-54 hierarchical state hash byte-identically on
#     Linux-x64 / Win-x64 / macOS-ARM64 (particle_world.cpp / components.h);
#   * FREE-RUNNING COSMETIC particles are a PRESENTATION OBSERVER (cosmetic.cpp) that reads sim state
#     read-only, holds its own float state, and folds into NO hash — the R-SIM-001 downstream-observer
#     rule (mirrors the audio-as-observer model). The one float TU (cosmetic.cpp) never touches the
#     sim path, so it cannot taint the deterministic build.
#
# It is a PACKAGE, not kernel core (R-KERNEL-001 stays minimal / L-60 microkernel invariant): it
# composes on the kernel World like its siblings (physics3d, physics2d, simmath) and the kernel gains
# NO dependency on it — nothing under src/kernel/ links context_particles.

add_library(context_particles STATIC
    src/particle_world.cpp
    src/cosmetic.cpp)

target_include_directories(context_particles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel + context_simmath: the public headers expose the kernel World/Entity (the ECS
# the package composes on) and the simmath fixed-point value types (EmitterDesc / ParticleConfig) — so
# consumers need both include dirs. The dependency direction is the package layer composing on the
# kernel; the kernel never links back (L-60). Particles do not collide, so — unlike the physics
# packages — this package has NO context_spatial dependency.
target_link_libraries(context_particles PUBLIC context_kernel context_simmath)

# PRIVATE context_session: the M6-F0b sim-component registration seam
# (register_package_sim_component / sim_components()) the particle components fold into the
# hierarchical state hash through — only particle_world.cpp (and the read-only cosmetic archetype
# walk) names it.
target_link_libraries(context_particles PRIVATE context_session)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_particles PRIVATE context_warnings)

# The test executables link context_session (hash_world / sim_components in assertions); on this MinGW
# host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's, so they static-link the
# GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC untouched; the same self-contained-binary
# rationale as runtime/session and the physics packages).
set(_context_particles_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/particles_test.h is the tiny CHECK() harness. Coverage:
#   - test_components: sim-component registration by stable name (fields, idempotency, pristine
#     builtin set untouched) + hash_world folding (identical-world equality, field sensitivity) + the
#     POD int64 layout law;
#   - test_simulation: fixed-point emit/age/despawn accounting (count == rate * min(step, lifetime)),
#     gravity integration (a spread-free particle falls), and within-run determinism (two identical
#     worlds hash identically at every tick);
#   - test_cosmetic: the R-SIM-001 / m6-exit-5 presentation-observer invariant — arbitrary cosmetic
#     activity leaves the sim state hash byte-identical, never writes sim state, and registers no sim
#     component, while doing real (non-vacuous) work;
#   - test_errors: the fail-closed particle.* refusals (invalid config / entity / step, missing
#     component set), the world left untouched on refusal, and the code strings pinned to the contract
#     catalog's particle.* block.
set(_context_particles_tests
    test_components
    test_simulation
    test_cosmetic
    test_errors)

foreach(_t IN LISTS _context_particles_tests)
    add_executable(context_particles_${_t} tests/${_t}.cpp)
    target_link_libraries(context_particles_${_t}
        PRIVATE context_particles context_session context_warnings)
    target_link_options(context_particles_${_t} PRIVATE ${_context_particles_static_runtime})
    target_include_directories(context_particles_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME particle-${_t} COMMAND context_particles_${_t})
endforeach()

# --- the M6 P4 particle DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, issue #178) ---------------
# A particle-active scene (three emitters — a fountain, a drifting stream, a spread-free trickle)
# stepped N fixed ticks through the REAL context_particles package, its per-tick hierarchical state
# hash (hash_world over sim_components()) folded and asserted against a cross-platform GOLDEN. Joins
# the `determinism-*` ctest family, so the blocking CI "Determinism gate" step (-R "^determinism-")
# runs it on all three build-matrix legs; its target is ALSO on the strict-FP `deterministic` job's
# hand-maintained --target list in .github/workflows/ci.yml. Integer-only sim state + fixed-endian
# hashing make the golden portable.
add_executable(context_particles_determinism_scene tests/determinism_particle_scene.cpp)
target_link_libraries(context_particles_determinism_scene
    PRIVATE context_particles context_session context_warnings)
target_link_options(context_particles_determinism_scene PRIVATE ${_context_particles_static_runtime})
target_include_directories(context_particles_determinism_scene
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME determinism-particle-scene COMMAND context_particles_determinism_scene)
