// Fixed-point TRS transform (R-SIM-008 math half, M6-F0b). Translation + rotation (quaternion) +
// non-uniform scale, all over Fixed — no alloc, no float, so a transformed point folds into the state
// hash bit-identically across the platform matrix. The one composed spatial primitive the physics /
// animation / spline packages share instead of each rolling its own float matrix path.

#pragma once

#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/vec.h"

namespace context::packages::simmath
{

struct Transform
{
    Vec3 translation{kZero, kZero, kZero};
    Quat rotation = quat_identity();
    Vec3 scale{kOne, kOne, kOne};
};

// The identity transform (no translation, identity rotation, unit scale).
[[nodiscard]] inline Transform transform_identity() noexcept { return Transform{}; }

[[nodiscard]] inline bool operator==(const Transform& a, const Transform& b) noexcept
{
    return a.translation == b.translation && a.rotation == b.rotation && a.scale == b.scale;
}

// Map a local-space point through the transform: scale, then rotate, then translate.
[[nodiscard]] inline Vec3 transform_point(const Transform& t, Vec3 p) noexcept
{
    return t.translation + rotate(t.rotation, hadamard(p, t.scale));
}

// Map a local-space DIRECTION (rotation only — translation and, for a foundation, uniform-scale-only
// semantics; non-uniform scale on a direction is intentionally not sheared here).
[[nodiscard]] inline Vec3 transform_direction(const Transform& t, Vec3 dir) noexcept
{
    return rotate(t.rotation, dir);
}

// Compose two transforms so transform_point(compose(parent, child), p) ==
// transform_point(parent, transform_point(child, p)) for the translation+rotation path.
[[nodiscard]] inline Transform compose(const Transform& parent, const Transform& child) noexcept
{
    Transform out;
    out.translation = transform_point(parent, child.translation);
    out.rotation = parent.rotation * child.rotation;
    out.scale = hadamard(parent.scale, child.scale);
    return out;
}

} // namespace context::packages::simmath
