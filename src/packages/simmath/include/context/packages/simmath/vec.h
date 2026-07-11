// Fixed-point 2D / 3D vectors (R-SIM-008 math half, M6-F0b). Plain value types over Fixed — no
// allocation, no float, so every vector operation folds into the deterministic state hash unchanged
// across the platform matrix. `length` / `normalized` route through the deterministic fixed_sqrt.

#pragma once

#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/trig.h"

namespace context::packages::simmath
{

struct Vec2
{
    Fixed x;
    Fixed y;
};

struct Vec3
{
    Fixed x;
    Fixed y;
    Fixed z;
};

// --- Vec2 -----------------------------------------------------------------------------------------

[[nodiscard]] constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 a) noexcept { return {-a.x, -a.y}; }
[[nodiscard]] constexpr Vec2 operator*(Vec2 a, Fixed s) noexcept { return {a.x * s, a.y * s}; }
[[nodiscard]] constexpr bool operator==(Vec2 a, Vec2 b) noexcept { return a.x == b.x && a.y == b.y; }

[[nodiscard]] constexpr Fixed dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] constexpr Fixed length_squared(Vec2 a) noexcept { return dot(a, a); }
[[nodiscard]] inline Fixed length(Vec2 a) noexcept { return fixed_sqrt(length_squared(a)); }

// Unit-length copy; the zero vector normalizes to itself (no divide-by-zero).
[[nodiscard]] inline Vec2 normalized(Vec2 a) noexcept
{
    const Fixed len = length(a);
    if (len.raw == 0)
        return a;
    return {a.x / len, a.y / len};
}

// --- Vec3 -----------------------------------------------------------------------------------------

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, Fixed s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] constexpr bool operator==(Vec3 a, Vec3 b) noexcept
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Component-wise (Hadamard) product — used to apply a non-uniform scale.
[[nodiscard]] constexpr Vec3 hadamard(Vec3 a, Vec3 b) noexcept
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

[[nodiscard]] constexpr Fixed dot(Vec3 a, Vec3 b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] constexpr Fixed length_squared(Vec3 a) noexcept { return dot(a, a); }
[[nodiscard]] inline Fixed length(Vec3 a) noexcept { return fixed_sqrt(length_squared(a)); }

[[nodiscard]] inline Vec3 normalized(Vec3 a) noexcept
{
    const Fixed len = length(a);
    if (len.raw == 0)
        return a;
    return {a.x / len, a.y / len, a.z / len};
}

} // namespace context::packages::simmath
