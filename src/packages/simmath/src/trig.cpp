// Deterministic transcendentals (see trig.h). Every routine is pure integer arithmetic: a bit-by-bit
// integer square root and fixed-point polynomial approximations with integer range reduction. No
// `std::sin`/`std::sqrt`/`libm` is called — that is the whole point (a platform libm result would
// diverge across the determinism matrix).

#include "context/packages/simmath/trig.h"

#include <cstdint>

namespace context::packages::simmath
{

namespace
{

// Integer square root of a non-negative 64-bit value (floor). Classic bit-by-bit ("digit by digit")
// algorithm — pure shifts, adds, compares, so it is bit-identical on every target.
[[nodiscard]] std::uint64_t isqrt_u64(std::uint64_t n) noexcept
{
    std::uint64_t result = 0;
    // Highest power of four <= n (start at bit 62 — the top even bit of a 64-bit value).
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > n)
        bit >>= 2;
    while (bit != 0)
    {
        if (n >= result + bit)
        {
            n -= result + bit;
            result = (result >> 1) + bit;
        }
        else
        {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

// sin(x) for x in [0, pi/2], via the factored Taylor series
//   sin(x) = x * (1 - x^2/6 * (1 - x^2/20 * (1 - x^2/42)))
// (terms up to x^7; the residual over [0, pi/2] is ~1.6e-4). Integer divides by the small odd
// constants are exact-truncating and deterministic.
[[nodiscard]] Fixed poly_sin_unit(Fixed x) noexcept
{
    const Fixed x2 = x * x;
    Fixed t = kOne - x2 / 42;
    t = kOne - (x2 * t) / 20;
    t = kOne - (x2 * t) / 6;
    return x * t;
}

// atan(z) for |z| <= 1, via a minimax odd polynomial (residual ~1e-4 on [-1, 1]):
//   atan(z) ~ z * (c0 - c2 z^2 + c4 z^4 - c6 z^6 + c8 z^8)
// coefficients in Q16.
constexpr Fixed kAtanC0 = Fixed::from_raw(65527); // 0.99986600
constexpr Fixed kAtanC2 = Fixed::from_raw(21649); // 0.33029953
constexpr Fixed kAtanC4 = Fixed::from_raw(11806); // 0.18014100
constexpr Fixed kAtanC6 = Fixed::from_raw(5580);  // 0.08513300
constexpr Fixed kAtanC8 = Fixed::from_raw(1366);  // 0.02083510

[[nodiscard]] Fixed poly_atan_unit(Fixed z) noexcept
{
    const Fixed z2 = z * z;
    Fixed p = kAtanC4 + z2 * (Fixed::from_raw(-kAtanC6.raw) + z2 * kAtanC8);
    p = kAtanC0 + z2 * (Fixed::from_raw(-kAtanC2.raw) + z2 * p);
    return z * p;
}

} // namespace

Fixed fixed_sqrt(Fixed x) noexcept
{
    if (x.raw <= 0)
        return kZero;
    // sqrt(value) in Q16 == isqrt(raw << kFractionBits): sqrt(raw/2^16) * 2^16 == sqrt(raw * 2^16).
    const std::uint64_t scaled = static_cast<std::uint64_t>(x.raw) << kFractionBits;
    return Fixed::from_raw(static_cast<std::int64_t>(isqrt_u64(scaled)));
}

Fixed fixed_sin(Fixed radians) noexcept
{
    // Reduce to [0, 2*pi) with integer modulo (deterministic; no float remainder).
    std::int64_t r = radians.raw % kTwoPi.raw;
    if (r < 0)
        r += kTwoPi.raw;
    const std::int64_t half = kHalfPi.raw;
    const std::int64_t pi = kPi.raw;
    const std::int64_t two_pi = kTwoPi.raw;

    // Fold each of the four quadrants onto [0, pi/2] with the sin symmetries.
    if (r <= half)
        return poly_sin_unit(Fixed::from_raw(r));
    if (r <= pi)
        return poly_sin_unit(Fixed::from_raw(pi - r));
    if (r <= pi + half)
        return -poly_sin_unit(Fixed::from_raw(r - pi));
    return -poly_sin_unit(Fixed::from_raw(two_pi - r));
}

Fixed fixed_cos(Fixed radians) noexcept
{
    // cos(x) == sin(x + pi/2); reuse the reduced sin.
    return fixed_sin(radians + kHalfPi);
}

Fixed fixed_tan(Fixed radians) noexcept
{
    const Fixed s = fixed_sin(radians);
    const Fixed c = fixed_cos(radians);
    if (c.raw == 0)
        // On the asymptote: saturate with the sign of sin rather than divide by zero.
        return s.raw >= 0 ? Fixed::from_raw(INT64_MAX) : Fixed::from_raw(INT64_MIN);
    return s / c;
}

Fixed fixed_atan2(Fixed y, Fixed x) noexcept
{
    if (x.raw == 0 && y.raw == 0)
        return kZero;

    const Fixed ax = fixed_abs(x);
    const Fixed ay = fixed_abs(y);
    Fixed angle;
    if (ax >= ay)
    {
        // |y/x| <= 1: the polynomial domain. y/x keeps the correct sign.
        angle = poly_atan_unit(y / x);
        if (x.raw < 0)
            angle = (y.raw >= 0) ? angle + kPi : angle - kPi;
    }
    else
    {
        // |x/y| < 1: use atan2(y, x) == +-pi/2 - atan(x/y).
        const Fixed inner = poly_atan_unit(x / y);
        angle = (y.raw > 0) ? kHalfPi - inner : -kHalfPi - inner;
    }
    return angle;
}

Fixed fixed_atan(Fixed x) noexcept
{
    // atan(x) == atan2(x, 1): reuses the octant-correct reciprocal handling for |x| > 1.
    return fixed_atan2(x, kOne);
}

} // namespace context::packages::simmath
