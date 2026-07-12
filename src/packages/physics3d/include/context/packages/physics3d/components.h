// The physics3d sim components (M6 P1, R-SYS-001) — each a POD of std::int64_t fields ONLY (the
// inherited M6 invariant, sim_component.h): the hierarchical state hash folds fixed-width big-endian
// integers, so a physics-active world's digest is bit-identical on Linux-x64 / Win-x64 / macOS-ARM64
// (L-54). Every field is a Q16 fixed-point raw value (simmath's kFractionBits scale) or a small
// integer enum/flag — never a float. The package registers these into the combined sim_components()
// registry by STABLE NAME (register_sim_components(), physics_world.h), so the digest is independent
// of first-touch ComponentId order across processes.

#pragma once

#include "context/packages/simmath/fixed.h"

#include <cstdint>

namespace context::packages::physics3d
{

// Position + orientation of a rigid body. px/py/pz are Q16 world units; qx/qy/qz/qw are the Q16
// components of a unit quaternion (default = identity).
struct Transform3d
{
    std::int64_t px = 0;
    std::int64_t py = 0;
    std::int64_t pz = 0;
    std::int64_t qx = 0;
    std::int64_t qy = 0;
    std::int64_t qz = 0;
    std::int64_t qw = simmath::kFixedOneRaw;
};

// Linear (vx/vy/vz, Q16 units/second) + angular (wx/wy/wz, Q16 radians/second) velocity.
struct Velocity3d
{
    std::int64_t vx = 0;
    std::int64_t vy = 0;
    std::int64_t vz = 0;
    std::int64_t wx = 0;
    std::int64_t wy = 0;
    std::int64_t wz = 0;
};

// Body flags values (the `flags` field of Body3d).
inline constexpr std::int64_t kBodyFlagDynamic = 0;
inline constexpr std::int64_t kBodyFlagStatic = 1;

// Mass + material properties. inv_mass / inv_inertia are Q16 (0 == static / infinite mass);
// inv_inertia is a UNIFORM scalar inverse inertia (a deterministic v1 simplification — a diagonal
// tensor is a documented future refinement). restitution / friction are Q16 in [0, 1].
struct Body3d
{
    std::int64_t inv_mass = 0;
    std::int64_t inv_inertia = 0;
    std::int64_t restitution = 0;
    std::int64_t friction = 0;
    std::int64_t flags = kBodyFlagStatic;
};

// Collider shape values (the `shape` field of Collider3d).
inline constexpr std::int64_t kShapeSphere = 0;
inline constexpr std::int64_t kShapeBox = 1;

// Collision shape. shape == kShapeSphere: ex is the radius (ey/ez unused, kept 0); shape ==
// kShapeBox: ex/ey/ez are the Q16 half-extents in the body frame.
struct Collider3d
{
    std::int64_t shape = kShapeSphere;
    std::int64_t ex = simmath::kFixedOneRaw;
    std::int64_t ey = 0;
    std::int64_t ez = 0;
};

// The stable registered names (sim_components() lookup keys — the cross-process hashing identity).
inline constexpr const char* kTransformComponentName = "physics3d_transform";
inline constexpr const char* kVelocityComponentName = "physics3d_velocity";
inline constexpr const char* kBodyComponentName = "physics3d_body";
inline constexpr const char* kColliderComponentName = "physics3d_collider";

} // namespace context::packages::physics3d
