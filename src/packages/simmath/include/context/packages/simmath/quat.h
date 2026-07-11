// Fixed-point quaternion (R-SIM-008 math half, M6-F0b). A rotation as (x, y, z, w) with w the scalar
// part. Value type over Fixed — no alloc, no float — so a rotated transform folds into the state hash
// bit-identically across the platform matrix. `from_axis_angle` routes through the deterministic
// fixed_sin/cos; `rotate` uses the trig-free cross-product form (no per-call transcendental).

#pragma once

#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/trig.h"
#include "context/packages/simmath/vec.h"

namespace context::packages::simmath
{

struct Quat
{
    Fixed x;
    Fixed y;
    Fixed z;
    Fixed w;
};

// The identity rotation (0, 0, 0, 1).
[[nodiscard]] constexpr Quat quat_identity() noexcept { return {kZero, kZero, kZero, kOne}; }

[[nodiscard]] constexpr bool operator==(Quat a, Quat b) noexcept
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// Hamilton product a*b (apply b then a when both are rotations).
[[nodiscard]] constexpr Quat operator*(Quat a, Quat b) noexcept
{
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

[[nodiscard]] constexpr Quat conjugate(Quat q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }
[[nodiscard]] constexpr Fixed dot(Quat a, Quat b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
[[nodiscard]] inline Fixed length(Quat q) noexcept { return fixed_sqrt(dot(q, q)); }

// Unit-length copy; a zero quaternion returns the identity rather than dividing by zero.
[[nodiscard]] inline Quat normalized(Quat q) noexcept
{
    const Fixed len = length(q);
    if (len.raw == 0)
        return quat_identity();
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

// A rotation of `angle` radians about `axis` (which SHOULD be unit length). Uses the deterministic
// fixed_sin/cos on the half-angle.
[[nodiscard]] inline Quat quat_from_axis_angle(Vec3 axis, Fixed angle) noexcept
{
    const Fixed half = angle / 2;
    const Fixed s = fixed_sin(half);
    return {axis.x * s, axis.y * s, axis.z * s, fixed_cos(half)};
}

// Rotate a vector by a quaternion via the cross-product form
//   v' = v + 2*w*(u x v) + 2*(u x (u x v))     (u = q.xyz)
// — no transcendental per call, purely algebraic and deterministic.
[[nodiscard]] inline Vec3 rotate(Quat q, Vec3 v) noexcept
{
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * Fixed::from_int(2);
    return v + t * q.w + cross(u, t);
}

} // namespace context::packages::simmath
