// The physics2d sim components (M6 P2, R-2D-002 / L-55) — each a POD of std::int64_t fields ONLY (the
// inherited M6 invariant, sim_component.h): the hierarchical state hash folds fixed-width big-endian
// integers, so a 2D physics-active world's digest is bit-identical on Linux-x64 / Win-x64 /
// macOS-ARM64 (L-54). Every field is a Q16 fixed-point raw value (simmath's kFractionBits scale) or a
// small integer enum/flag — never a float. The package registers these into the combined
// sim_components() registry by STABLE NAME (register_sim_components(), physics_world.h), so the digest
// is independent of first-touch ComponentId order across processes. Mirrors packages/physics3d in 2D:
// orientation collapses from a quaternion to a single scalar angle, angular velocity from a vector to
// a scalar, and colliders from sphere/box to circle/box.

#pragma once

#include "context/packages/simmath/fixed.h"

#include <cstdint>

namespace context::packages::physics2d
{

// Position + orientation of a rigid body. px/py are Q16 world units; angle is the Q16 rotation about
// the out-of-plane axis in radians (default = 0, i.e. unrotated).
struct Transform2d
{
    std::int64_t px = 0;
    std::int64_t py = 0;
    std::int64_t angle = 0;
};

// Linear (vx/vy, Q16 units/second) + angular (w, Q16 radians/second about the out-of-plane axis)
// velocity. Angular velocity is a signed scalar in 2D (positive = counter-clockwise).
struct Velocity2d
{
    std::int64_t vx = 0;
    std::int64_t vy = 0;
    std::int64_t w = 0;
};

// Body flags values (the `flags` field of Body2d).
inline constexpr std::int64_t kBodyFlagDynamic = 0;
inline constexpr std::int64_t kBodyFlagStatic = 1;

// Mass + material properties. inv_mass / inv_inertia are Q16 (0 == static / infinite mass);
// inv_inertia is the inverse of the body's scalar moment of inertia about its center (a single scalar
// in 2D — no tensor). restitution / friction are Q16 in [0, 1].
struct Body2d
{
    std::int64_t inv_mass = 0;
    std::int64_t inv_inertia = 0;
    std::int64_t restitution = 0;
    std::int64_t friction = 0;
    std::int64_t flags = kBodyFlagStatic;
};

// Collider shape values (the `shape` field of Collider2d).
inline constexpr std::int64_t kShapeCircle = 0;
inline constexpr std::int64_t kShapeBox = 1;

// Collision shape. shape == kShapeCircle: ex is the radius (ey unused, kept 0); shape == kShapeBox:
// ex/ey are the Q16 half-extents in the body frame.
struct Collider2d
{
    std::int64_t shape = kShapeCircle;
    std::int64_t ex = simmath::kFixedOneRaw;
    std::int64_t ey = 0;
};

// The stable registered names (sim_components() lookup keys — the cross-process hashing identity).
inline constexpr const char* kTransformComponentName = "physics2d_transform";
inline constexpr const char* kVelocityComponentName = "physics2d_velocity";
inline constexpr const char* kBodyComponentName = "physics2d_body";
inline constexpr const char* kColliderComponentName = "physics2d_collider";

} // namespace context::packages::physics2d
