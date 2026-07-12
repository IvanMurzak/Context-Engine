// The spline sim components (M6 P5, R-SYS-004) — each a POD of std::int64_t fields ONLY (the inherited
// M6 invariant, sim_component.h): the hierarchical state hash folds fixed-width big-endian integers, so
// a spline-active world's digest is bit-identical on Linux-x64 / Win-x64 / macOS-ARM64 (L-54). Every
// field is a Q16 fixed-point raw value (simmath's kFractionBits scale) or a small integer index/flag —
// never a float. The package registers these into the combined sim_components() registry by STABLE NAME
// (register_sim_components(), spline_world.h), so the digest is independent of first-touch ComponentId
// order across processes.
//
// This is the DETERMINISTIC SIM-PATH spline component ONLY: an entity that follows a spline path
// (PathFollower). The tooling/geometry DISPLAY tessellation is a presentation observer (R-SIM-001,
// cosmetic_curve.h) that lives OFF this sim path — it holds its own float display geometry, registers
// NO sim component here, and never folds into the hash.

#pragma once

#include <cstdint>

namespace context::packages::spline
{

// One entity following a deterministic spline path. `path` indexes the SplineWorld's installed curves;
// `distance` is the Q16 arc length traveled along that path; `speed` is the Q16 world-units/second
// advance rate; `loop` is a boolean (0 = clamp at the path end, 1 = wrap to the start) stored as an
// int64 so it folds into the hash. `px`/`py`/`pz` are the Q16 evaluated world position and `heading`
// is the Q16 yaw (radians) derived from the path tangent — all recomputed each step from `distance`,
// so a spline-following entity folds into the L-54 hierarchical hash. Integer end to end.
struct PathFollower
{
    std::int64_t path = 0;     // active path index (into the SplineWorld's installed curves)
    std::int64_t distance = 0; // Q16 arc length traveled along the path
    std::int64_t speed = 0;    // Q16 world-units/second advance rate
    std::int64_t loop = 0;     // 0 = clamp at the end, 1 = wrap to the start
    std::int64_t px = 0;       // Q16 evaluated world position x
    std::int64_t py = 0;       // Q16 evaluated world position y
    std::int64_t pz = 0;       // Q16 evaluated world position z
    std::int64_t heading = 0;  // Q16 yaw (radians) from the path tangent
};

// The stable registered name (sim_components() lookup key — the cross-process hashing identity).
inline constexpr const char* kFollowerComponentName = "spline_follower";

} // namespace context::packages::spline
