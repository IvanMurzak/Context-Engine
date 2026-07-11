// Deterministic transcendental library — the single shipped sin / cos / tan / sqrt / atan / atan2 the
// whole sim path uses (R-SIM-008, M6-F0b). Integer-only by construction: NO platform `libm` is ever
// called on the sim path (calling `std::sin` would reintroduce a per-platform float result and break
// the cross-platform state hash), so each function is computed from Fixed integer arithmetic —
// bit-by-bit integer sqrt and fixed-point polynomial approximations with integer range reduction.
// Because the whole computation is integer add / mul / shift, the result is bit-identical on x86-64
// and arm64: the same digest on every leg of the L-54 determinism matrix (Linux-x64 / Win-x64 /
// macOS-ARM64). Accuracy is a few parts in 10^4 over the working range — deterministic first, precise
// second, which is exactly the trade the integer-only sim contract demands.

#pragma once

#include "context/packages/simmath/fixed.h"

namespace context::packages::simmath
{

// Fixed-point circle constants (Q16). Derived from the mathematical values, rounded to the nearest
// sub-unit — the only "magic numbers" the range reduction below relies on.
inline constexpr Fixed kPi = Fixed::from_raw(205887);     // pi      == 3.14159265 * 2^16
inline constexpr Fixed kTwoPi = Fixed::from_raw(411775);  // 2*pi    == 6.28318531 * 2^16
inline constexpr Fixed kHalfPi = Fixed::from_raw(102944); // pi/2    == 1.57079633 * 2^16

// Non-negative square root of `x` (x < 0 clamps to 0). Bit-by-bit integer sqrt over the up-shifted
// mantissa — exact to the last sub-unit, no float.
[[nodiscard]] Fixed fixed_sqrt(Fixed x) noexcept;

// sin / cos of an angle in radians. Full-range: the argument is reduced modulo 2*pi with integer
// arithmetic before a quadrant-local polynomial is evaluated.
[[nodiscard]] Fixed fixed_sin(Fixed radians) noexcept;
[[nodiscard]] Fixed fixed_cos(Fixed radians) noexcept;

// tan == sin/cos. Near an odd multiple of pi/2 (cos ~ 0) the result saturates rather than dividing by
// zero; callers on the sim path should avoid the asymptote.
[[nodiscard]] Fixed fixed_tan(Fixed radians) noexcept;

// atan(x): the angle in (-pi/2, pi/2) whose tangent is x. Defined for the whole real line via the
// reciprocal identity for |x| > 1.
[[nodiscard]] Fixed fixed_atan(Fixed x) noexcept;

// atan2(y, x): the full [-pi, pi] angle of the vector (x, y), quadrant-correct. atan2(0, 0) == 0.
[[nodiscard]] Fixed fixed_atan2(Fixed y, Fixed x) noexcept;

} // namespace context::packages::simmath
