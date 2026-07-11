# src/packages/simmath/ — the shared no-alloc DETERMINISTIC sim-math library (R-SIM-008 math half,
# M6-F0b, issue #172). The ONE fixed-point vector / quaternion / transform + deterministic
# transcendental (sin/cos/tan/sqrt/atan/atan2) library every sim package (physics, animation,
# particles, spline) composes on, so no package rolls its own float math path. Integer-only by law: a
# Fixed is an int64 in Q16 sub-units and every operation is integer add/mul/shift, so a result is
# bit-identical on x86-64 and arm64 — it folds into the L-54 hierarchical state hash unchanged across
# the determinism matrix, and is deterministic under the `deterministic` preset (no platform libm on
# the sim path; the transcendentals are computed from integer polynomials, never std::sin).
#
# It is a PACKAGE, not kernel core (R-KERNEL-001 stays minimal / L-60 microkernel invariant): it
# composes on the kernel like every sim package that consumes it, and the kernel gains NO dependency on
# it — nothing under src/kernel/ links context_simmath.

add_library(context_simmath STATIC
    src/trig.cpp)

target_include_directories(context_simmath PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel: simmath is a first-party package under the established package pattern (the
# spatial precedent) — the sim packages that consume its types compose them onto the kernel World, so
# simmath sits in the package layer that depends on the kernel; the kernel never links back (the
# microkernel invariant, L-60 / R-KERNEL-001).
target_link_libraries(context_simmath PUBLIC context_kernel)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_simmath PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/simmath_test.h is the tiny CHECK() harness (+ a TEST-ONLY
# to_double / libm tolerance oracle — the LIBRARY never touches float). Coverage:
#   - test_fixed: Q16 arithmetic (add/sub/mul/div, integer overloads), floor/trunc rounding, helpers,
#     and bit-identical determinism of an operand stream;
#   - test_trig: sqrt / sin / cos / tan / atan / atan2 vs a libm reference within a Q16 tolerance,
#     full-range reduction (negative + multi-period), quadrant/axis atan2 cases, and determinism;
#   - test_vec: Vec2/Vec3 arithmetic, dot/cross, length/normalize (via fixed_sqrt), determinism;
#   - test_quat_transform: quaternion identity/product/conjugate/from_axis_angle/rotate, TRS
#     transform_point/compose, and determinism.
set(_context_simmath_tests
    test_fixed
    test_trig
    test_vec
    test_quat_transform)

foreach(_t IN LISTS _context_simmath_tests)
    add_executable(context_simmath_${_t} tests/${_t}.cpp)
    target_link_libraries(context_simmath_${_t} PRIVATE context_simmath context_warnings)
    target_include_directories(context_simmath_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME simmath-${_t} COMMAND context_simmath_${_t})
endforeach()
