// Deterministic fixed-point scalar — the integer-only real-number substitute for the sim path
// (R-SIM-008 math half, M6-F0b). No float EVER appears here: a Fixed is an int64 in Q<kFractionBits>
// sub-units, and every operation below is integer add / mul / shift / compare, so a Fixed's bit
// pattern — and every result computed from it — is bit-identical on x86-64 and arm64. That is the
// cross-platform determinism law the state hash rests on (hash.h / L-54): the simmath library exists
// so every sim package (physics, animation, particles, spline) shares ONE deterministic math core
// instead of each risking a platform-divergent float path.
//
// Rounding: multiply and integer-divide TRUNCATE toward negative infinity (an arithmetic right shift
// / arithmetic integer division), identically on every target — a pure integer operation, never an FP
// rounding mode. Domain: operands are expected within ±2^31 raw sub-units (±32768 in value); the int64
// product of two such values stays within 2^63, so the fixed-point multiply never overflows in the sim
// magnitude envelope (the same bound the F0a physics-determinism spike documents).

#pragma once

#include <cstdint>

namespace context::packages::simmath
{

// Number of fractional bits in a Fixed (Q16): the represented value is raw / 2^16. 16 matches the F0a
// physics-determinism spike so every sim package shares one sub-unit scale.
inline constexpr int kFractionBits = 16;
inline constexpr std::int64_t kFixedOneRaw = std::int64_t{1} << kFractionBits;

// A fixed-point scalar. `raw` is the underlying int64; value == raw / 2^kFractionBits.
struct Fixed
{
    std::int64_t raw = 0;

    constexpr Fixed() noexcept = default;

    // Construct from the raw Q16 integer directly (no scaling).
    [[nodiscard]] static constexpr Fixed from_raw(std::int64_t r) noexcept
    {
        Fixed f;
        f.raw = r;
        return f;
    }

    // Construct from a whole integer value (v << kFractionBits).
    [[nodiscard]] static constexpr Fixed from_int(std::int64_t v) noexcept
    {
        return from_raw(v << kFractionBits);
    }

    // Exact fixed-point of the rational num/den (integer division of the scaled numerator) — the
    // deterministic, float-free way to spell a fractional literal (e.g. from_ratio(1, 2) == 0.5).
    [[nodiscard]] static constexpr Fixed from_ratio(std::int64_t num, std::int64_t den) noexcept
    {
        return from_raw((num << kFractionBits) / den);
    }

    // Floor toward negative infinity (arithmetic shift): floor(1.5) == 1, floor(-1.5) == -2.
    [[nodiscard]] constexpr std::int64_t floor_int() const noexcept { return raw >> kFractionBits; }

    // Truncate toward zero: trunc(1.5) == 1, trunc(-1.5) == -1.
    [[nodiscard]] constexpr std::int64_t trunc_int() const noexcept { return raw / kFixedOneRaw; }
};

inline constexpr Fixed kZero = Fixed::from_raw(0);
inline constexpr Fixed kOne = Fixed::from_raw(kFixedOneRaw);

// --- arithmetic (every op is pure integer, deterministic across platforms) ------------------------

[[nodiscard]] constexpr Fixed operator+(Fixed a, Fixed b) noexcept
{
    return Fixed::from_raw(a.raw + b.raw);
}
[[nodiscard]] constexpr Fixed operator-(Fixed a, Fixed b) noexcept
{
    return Fixed::from_raw(a.raw - b.raw);
}
[[nodiscard]] constexpr Fixed operator-(Fixed a) noexcept { return Fixed::from_raw(-a.raw); }

// Fixed * Fixed: the int64 product shifted down by the fractional bits (floor rounding). Deterministic
// arithmetic right shift on the signed product (C++20 mandates two's-complement arithmetic shift).
[[nodiscard]] constexpr Fixed operator*(Fixed a, Fixed b) noexcept
{
    return Fixed::from_raw((a.raw * b.raw) >> kFractionBits);
}
// Fixed / Fixed: scale the numerator up by the fractional bits before the integer divide.
[[nodiscard]] constexpr Fixed operator/(Fixed a, Fixed b) noexcept
{
    return Fixed::from_raw((a.raw << kFractionBits) / b.raw);
}

// Fixed scaled by a whole integer (exact — no shift): value * n and value / n.
[[nodiscard]] constexpr Fixed operator*(Fixed a, std::int64_t n) noexcept
{
    return Fixed::from_raw(a.raw * n);
}
[[nodiscard]] constexpr Fixed operator*(std::int64_t n, Fixed a) noexcept { return a * n; }
[[nodiscard]] constexpr Fixed operator/(Fixed a, std::int64_t n) noexcept
{
    return Fixed::from_raw(a.raw / n);
}

constexpr Fixed& operator+=(Fixed& a, Fixed b) noexcept { return a = a + b; }
constexpr Fixed& operator-=(Fixed& a, Fixed b) noexcept { return a = a - b; }
constexpr Fixed& operator*=(Fixed& a, Fixed b) noexcept { return a = a * b; }
constexpr Fixed& operator/=(Fixed& a, Fixed b) noexcept { return a = a / b; }

// --- comparisons ----------------------------------------------------------------------------------

[[nodiscard]] constexpr bool operator==(Fixed a, Fixed b) noexcept { return a.raw == b.raw; }
[[nodiscard]] constexpr bool operator!=(Fixed a, Fixed b) noexcept { return a.raw != b.raw; }
[[nodiscard]] constexpr bool operator<(Fixed a, Fixed b) noexcept { return a.raw < b.raw; }
[[nodiscard]] constexpr bool operator<=(Fixed a, Fixed b) noexcept { return a.raw <= b.raw; }
[[nodiscard]] constexpr bool operator>(Fixed a, Fixed b) noexcept { return a.raw > b.raw; }
[[nodiscard]] constexpr bool operator>=(Fixed a, Fixed b) noexcept { return a.raw >= b.raw; }

// --- helpers --------------------------------------------------------------------------------------

[[nodiscard]] constexpr Fixed fixed_abs(Fixed a) noexcept
{
    return a.raw < 0 ? Fixed::from_raw(-a.raw) : a;
}
[[nodiscard]] constexpr Fixed fixed_min(Fixed a, Fixed b) noexcept { return a < b ? a : b; }
[[nodiscard]] constexpr Fixed fixed_max(Fixed a, Fixed b) noexcept { return a > b ? a : b; }
[[nodiscard]] constexpr Fixed fixed_clamp(Fixed v, Fixed lo, Fixed hi) noexcept
{
    return fixed_min(fixed_max(v, lo), hi);
}
// -1 / 0 / +1 sign of a Fixed.
[[nodiscard]] constexpr std::int64_t fixed_sign(Fixed a) noexcept
{
    return a.raw > 0 ? 1 : (a.raw < 0 ? -1 : 0);
}

} // namespace context::packages::simmath
