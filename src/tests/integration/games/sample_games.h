// The M6 EXIT sample-game runtime mirrors (issue #194): each game under samples/ registers as a
// first-class session SCENARIO (session.h § scenario registry), so the smoke gates drive it through
// the REAL RuntimeKernel session surface — simTick, the ordinary injection sink, the hierarchical
// state hash — headless, GPU-free, audio on the null backend (R-HEAD-002, R-QA-005).
//
// The authored sample project is the source of truth for the entity LAYOUT: each scenario factory
// PARSES its game's scene file (entity names + integer transform positions, converted losslessly to
// Q16 fixed-point) and builds the sim world from it, so renaming/moving an authored entity fails the
// smoke ctest — the sample cannot rot silently past schema validity (R-QA-006 / R-BUILD-009).
// Collider shapes/sizes, masses, materials, rigs, and the fixed per-tick system order are the
// runtime half, mirrored here from the authored kind files (the same authored↔runtime mirror the
// package determinism scenes established for samples/anim-graphs/, samples/input-bindings/, …).
//
// Test-support layer: this library sits ABOVE the runtime session and the feature packages (it links
// both), exactly like the session-tests' combined gameplay gate — the context_session LIBRARY gains
// no package dependency (L-60 layering unchanged).

#pragma once

#include "context/kernel/entity.h"
#include "context/kernel/world.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::tests::games
{

// --- shared: authored scene layout ----------------------------------------------------------------

// One authored scene entity the layout parser exposes: its name and its integer world position
// (authored positions in the game scenes are INTEGERS by convention — exactly representable in Q16,
// so the fixed-point conversion is lossless; the parser rejects anything else).
struct NamedPosition
{
    std::string name;
    std::int64_t x = 0; // whole world units (meters); Q16 raw is value << 16
    std::int64_t y = 0;
    std::int64_t z = 0;
};

// Parse `scene_path` (a ctx:scene document) and extract every entity's (name, integer position).
// Returns false (with a stderr diagnostic) on a missing file, a parse failure, or a non-integer
// position component — the fail-closed path a rotted sample surfaces through.
[[nodiscard]] bool load_scene_layout(const std::string& scene_path,
                                     std::vector<NamedPosition>& out);

// Find `name` in a parsed layout. Returns nullptr when absent.
[[nodiscard]] const NamedPosition* find_entity(const std::vector<NamedPosition>& layout,
                                               const std::string& name);

// Rebuild a kernel handle from the two int64 halves a game-state component stores.
[[nodiscard]] inline kernel::Entity entity_from(std::int64_t index, std::int64_t generation)
{
    return kernel::Entity{static_cast<std::uint32_t>(index),
                          static_cast<std::uint32_t>(generation)};
}

// Read a world-singleton sim component out of a (const) world by value (all-defaults when absent).
// World::each has no const overload, so the read-only walk casts away const — benign; this is the
// one place both game mirrors funnel their singleton game-state read-back through.
template <typename T>
[[nodiscard]] T read_singleton(const kernel::World& world)
{
    T out;
    kernel::World& mutable_world = const_cast<kernel::World&>(world);
    mutable_world.each<T>([&](kernel::Entity, T& value) { out = value; });
    return out;
}

// --- roll-3d --------------------------------------------------------------------------------------

// The roll-3d gameplay singleton sim component (integer-only, registered by stable name so it folds
// into the hierarchical state hash): the key entity handles (as int64 halves — components carry
// int64 fields only), the accumulated ball floor-impact count driving the particle burst + the
// impact sound + the flag's anim-graph parameter, and the burst/impact bookkeeping.
struct RollGame
{
    std::int64_t ball_index = 0;
    std::int64_t ball_gen = 0;
    std::int64_t prop_index = 0;
    std::int64_t prop_gen = 0;
    std::int64_t burst_index = 0;
    std::int64_t burst_gen = 0;
    std::int64_t boulder_a_index = 0;
    std::int64_t boulder_a_gen = 0;
    std::int64_t boulder_b_index = 0;
    std::int64_t boulder_b_gen = 0;
    std::int64_t impacts = 0;         // ball floor-impacts recorded by the impact system
    std::int64_t burst_ticks_left = 0; // burst emitter stays open this many more ticks
    std::int64_t prev_ball_vy = 0;    // last tick's ball vy (impact edge detection)
};

inline constexpr const char* kRollGameComponentName = "roll_game";

// The L-33 stable ids of samples/roll-3d/input/roll.input-bindings.json — the mapped action ids the
// router emits and the roll-3d input system folds into the sim InputState (see that file's notes for
// the human labels).
inline constexpr const char* kRollActionMoveX = "25612509fa7530cb"; // move_x — x-axis roll force
inline constexpr const char* kRollActionMoveY = "37ca9bc636e69ac2"; // move_y — z-axis roll force
inline constexpr const char* kRollActionUiMenu = "a8e9ae10fef110ab"; // ui_menu (pause)
inline constexpr const char* kRollContextGameplay = "8a804def3881cb2b";
inline constexpr const char* kRollContextPause = "0a7ae9fb3d0dd76e";

// The anim-graph mirror of samples/roll-3d/anim/flag-wave.anim-graph.json: state indices + the
// impact-count threshold that gates idle -> wave.
inline constexpr int kRollFlagStateIdle = 0;
inline constexpr int kRollFlagStateWave = 1;
inline constexpr std::int64_t kRollFlagWaveThreshold = 3;

// Register the "roll-3d" scenario with the session scenario registry, parsing the authored layout
// from `<samples_dir>/roll-3d/scenes/arena.scene.json` on every factory run. Idempotent.
void register_roll3d_scenario(const std::string& samples_dir);

// Read the RollGame singleton out of a world (all-defaults when absent — ball_gen 0 == invalid).
[[nodiscard]] RollGame read_roll_game(const kernel::World& world);

// --- platformer-2d ---------------------------------------------------------------------------------

// The platformer gameplay singleton sim component (integer-only, hash-folded by stable name): the
// key entity handles, the authored coin position, and the run/jump/landing/pickup bookkeeping the
// gameplay systems maintain (the mirror of what scripts/movement.ts authors in TypeScript).
struct PlatformerGame
{
    std::int64_t player_index = 0;
    std::int64_t player_gen = 0;
    std::int64_t crate_index = 0;
    std::int64_t crate_gen = 0;
    std::int64_t barrel_index = 0;
    std::int64_t barrel_gen = 0;
    std::int64_t puff_index = 0;
    std::int64_t puff_gen = 0;
    std::int64_t coin_x = 0;         // authored coin position (raw Q16)
    std::int64_t coin_y = 0;
    std::int64_t coin_collected = 0; // 0/1 — set once when the player overlaps the coin
    std::int64_t jumps = 0;          // grounded jump presses the control system converted
    std::int64_t landings = 0;       // fast-fall arrests the gameplay system recorded (puffs)
    std::int64_t puff_ticks_left = 0;
    std::int64_t prev_player_vy = 0; // last tick's player vy (landing edge detection)
};

inline constexpr const char* kPlatformerGameComponentName = "platformer_game";

// The L-33 stable ids of samples/platformer-2d/input/player.input-bindings.json (see that file's
// notes for the human labels).
inline constexpr const char* kPlatActionMoveX = "e8112cc9ea062172"; // move_x — run axis
inline constexpr const char* kPlatActionJump = "a2bf29db816e5b80"; // jump
inline constexpr const char* kPlatActionUiMenu = "7c26f13c5d642bb5"; // ui_menu (pause)
inline constexpr const char* kPlatContextGameplay = "a8d87ef64cc30231";
inline constexpr const char* kPlatContextPause = "bd690d74260afb9b";

// The anim-graph mirror of samples/platformer-2d/anim/player-sprite.anim-graph.json: state indices
// + the |vx| whole-units threshold that gates idle -> run.
inline constexpr int kPlatAnimStateIdle = 0;
inline constexpr int kPlatAnimStateRun = 1;
inline constexpr std::int64_t kPlatAnimRunThreshold = 1;

// Register the "platformer-2d" scenario with the session scenario registry, parsing the authored
// layout from `<samples_dir>/platformer-2d/scenes/level-1.scene.json` on every factory run.
// Idempotent.
void register_platformer2d_scenario(const std::string& samples_dir);

// Read the PlatformerGame singleton out of a world (all-defaults when absent — player_gen 0 ==
// invalid).
[[nodiscard]] PlatformerGame read_platformer_game(const kernel::World& world);

} // namespace context::tests::games
