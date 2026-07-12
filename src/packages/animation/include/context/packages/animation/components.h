// The animation sim components (M6 P3, R-SYS-002 / R-SYS-008) — each a POD of std::int64_t fields
// ONLY (the inherited M6 invariant, sim_component.h): the hierarchical state hash folds fixed-width
// big-endian integers, so an animation-active world's digest is bit-identical on Linux-x64 /
// Win-x64 / macOS-ARM64 (L-54). Every field is a Q16 fixed-point raw value (simmath's kFractionBits
// scale) or a small integer index — never a float. The package registers these into the combined
// sim_components() registry by STABLE NAME (register_sim_components(), animation_world.h), so the
// digest is independent of first-touch ComponentId order across processes.
//
// These are the DETERMINISTIC SIM-PATH animation components ONLY: the anim-graph playback cursor +
// transition blend (Animator) and the accumulated root-motion transform it drives (RootMotion). The
// FULL skeletal pose is evaluated cosmetically by a presentation observer (R-SIM-001, cosmetic_pose.h)
// that lives OFF this sim path — it holds its own float state, registers NO sim component here, and
// never folds into the hash.

#pragma once

#include "context/packages/simmath/fixed.h"

#include <cstdint>

namespace context::packages::animation
{

// One entity's deterministic animation cursor + anim-graph state + transition blend. `state`/`clip`
// index into the rig's graph-states / clips (animation_world.h). `time`/`from_time` are Q16 playhead
// seconds; `blend` is the Q16 transition progress in [0, kOne] (kOne == fully settled into `clip`);
// `blend_len` is the Q16 duration (seconds) of the active cross-fade; `param` is the Q16 control input
// the graph transitions on (e.g. locomotion speed). All integer — an animated entity folds into the
// L-54 hierarchical hash.
struct Animator
{
    std::int64_t state = 0;                                // active anim-graph state index
    std::int64_t clip = 0;                                 // active clip index (the state's clip)
    std::int64_t time = 0;                                 // Q16 playhead seconds into `clip`
    std::int64_t from_clip = 0;                            // clip being blended OUT of (== clip when settled)
    std::int64_t from_time = 0;                            // Q16 playhead into `from_clip`
    std::int64_t blend = simmath::kFixedOneRaw;            // Q16 transition progress [0, kOne]
    std::int64_t blend_len = 0;                            // Q16 active cross-fade duration (seconds)
    std::int64_t param = 0;                                // Q16 control parameter the graph transitions on
};

// The accumulated deterministic world root transform the root-motion track drives. px/py/pz are Q16
// world-unit positions; yaw is the Q16 accumulated facing angle (radians). Integer end to end, so it
// folds into the L-54 hierarchical hash.
struct RootMotion
{
    std::int64_t px = 0;
    std::int64_t py = 0;
    std::int64_t pz = 0;
    std::int64_t yaw = 0;
};

// The stable registered names (sim_components() lookup keys — the cross-process hashing identity).
inline constexpr const char* kAnimatorComponentName = "anim_animator";
inline constexpr const char* kRootMotionComponentName = "anim_root_motion";

} // namespace context::packages::animation
