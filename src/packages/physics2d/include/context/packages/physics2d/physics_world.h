// PhysicsWorld2d — the deterministic fixed-point rigid-body 2D physics stepper (M6 P2, R-2D-002 /
// L-55, implementing the F0a physics-core decision: docs/physics-determinism-decision.md).
//
// The 2D sibling of PhysicsWorld3d (packages/physics3d): a Box2D-class rigid-body simulation in the
// XY plane. Simulation state is integer / fixed-point ONLY (the components.h PODs — Q16 int64 fields),
// and every sim-affecting operation is simmath integer arithmetic (add / mul / shift / compare, the
// deterministic integer-only fixed_sin/fixed_cos for rotation, fixed_sqrt) — no float, no platform
// libm, no FMA — so a physics-active world's hierarchical state hash (R-QA-005 / L-54) is
// byte-identical on Linux-x64 / Win-x64 / macOS-ARM64. Decoupled from render: the package composes on
// the kernel World like any other package (L-60 — the kernel never links back) and touches no
// render/presentation state.
//
// Broad-phase: the shared `context_spatial` dynamic AABB tree prunes candidate pairs. The index is 3D
// and float-based; 2D uses a DEGENERATE-Z band (every body spans the same fixed z-slab, so the prune
// reduces to XY overlap) and determinism is preserved STRUCTURALLY exactly as in physics3d: fixed-point
// bounds are inflated by a conservative margin (strictly larger than the worst int64->float conversion
// error over the sim envelope) before conversion, so the float candidate set is a SUPERSET of the exact
// fixed-point overlap set on every platform, candidate pairs are re-decided by the EXACT fixed-point
// narrow phase, and pairs are processed in a sorted, entity-id-deterministic order (never spatial query
// order). The float prune can therefore only ever discard pairs the fixed-point narrow phase would also
// reject — it never decides sim state.
//
// v1 narrow-phase scope: circle-circle and circle-box contacts (a box may be static or dynamic).
// Box-box pairs are broad-phase detected but produce no contact in v1 (documented — the DoD scene is
// dynamic circles + a pushable box on static box platforms/ramps). Inertia is the exact 2D scalar
// moment (circle / box formula — a true scalar in 2D, not a simplification).

#pragma once

#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/packages/physics2d/components.h"
#include "context/packages/physics2d/errors.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"

#include <cstddef>
#include <memory>

namespace context::packages::physics2d
{

// Register the physics2d sim components into the combined sim_components() registry by stable name
// (sim_component.h, M6-F0b) so a physics-active world folds into the hierarchical state hash portably.
// Idempotent (re-registration overwrites); called automatically by PhysicsWorld2d's constructor and
// add_body, so any world built through this API hashes by name.
void register_sim_components();

// The collider shape of a body description.
enum class Shape : std::int64_t
{
    Circle = kShapeCircle,
    Box = kShapeBox,
};

// A body description consumed by PhysicsWorld2d::add_body. All quantities are simmath fixed-point.
struct BodyDesc
{
    simmath::Vec2 position{};
    simmath::Fixed angle{};              // rotation about the out-of-plane axis, radians
    simmath::Vec2 velocity{};
    simmath::Fixed angular_velocity{};   // signed scalar, radians/second (CCW positive)
    simmath::Fixed mass = simmath::kOne; // dynamic bodies: must be > 0 (kInvalidMassCode otherwise)
    simmath::Fixed restitution{};        // clamped into [0, 1]
    simmath::Fixed friction{};           // clamped into [0, 1]
    bool is_static = false;              // static bodies never move (infinite mass)
    Shape shape = Shape::Circle;
    // Circle: x is the radius (y ignored). Box: the half-extents in the body frame. Every used extent
    // must be > 0 (kInvalidShapeCode otherwise).
    simmath::Vec2 half_extents = {simmath::kOne, simmath::kOne};
};

// Global simulation configuration. Fixed-point end to end.
struct PhysicsConfig
{
    simmath::Vec2 gravity = {simmath::kZero, simmath::Fixed::from_int(-10)};
    simmath::Fixed linear_damping{}; // per-second velocity damping in [0, 1); 0 == none
    int solver_iterations = 8;       // velocity-impulse iterations per step
};

// A read-back snapshot of one body's fixed-point state (read_body below).
struct BodyState
{
    simmath::Vec2 position{};
    simmath::Fixed angle{};
    simmath::Vec2 velocity{};
    simmath::Fixed angular_velocity{};
    bool is_static = false;
};

// The physics stepper. Owns the broad-phase index; move-only (like the kernel World / SpatialIndex).
// Every method that can fail returns nullptr on success or one of the errors.h code strings — the
// same strings the contract error catalog registers in the physics2d.* block.
class PhysicsWorld2d
{
public:
    explicit PhysicsWorld2d(const PhysicsConfig& config = {});
    ~PhysicsWorld2d();

    PhysicsWorld2d(PhysicsWorld2d&&) noexcept;
    PhysicsWorld2d& operator=(PhysicsWorld2d&&) noexcept;
    PhysicsWorld2d(const PhysicsWorld2d&) = delete;
    PhysicsWorld2d& operator=(const PhysicsWorld2d&) = delete;

    // Attach the full physics component set (Transform2d + Velocity2d + Body2d + Collider2d) to `e`
    // per `desc`. Fail-closed validation BEFORE any component is added: a dead/null entity
    // (kInvalidEntityCode), a non-positive used shape extent (kInvalidShapeCode), or a non-positive
    // dynamic mass (kInvalidMassCode) rejects the body and leaves `world` untouched.
    const char* add_body(kernel::World& world, kernel::Entity e, const BodyDesc& desc);

    // Detach the physics component set from `e` and drop it from the broad-phase index.
    // kInvalidEntityCode for a dead/null handle; kMissingComponentCode if `e` is not a physics body.
    const char* remove_body(kernel::World& world, kernel::Entity e);

    // Apply an instantaneous center-of-mass impulse (Q16 mass*units/second) to a dynamic body.
    // Static bodies are a deterministic no-op (nullptr). kInvalidEntityCode / kMissingComponentCode
    // as above.
    const char* apply_impulse(kernel::World& world, kernel::Entity e, simmath::Vec2 impulse);

    // Advance every physics body in `world` by one fixed tick of `dt` (must be > 0, else
    // kInvalidStepCode): integrate gravity + damping + velocities (semi-implicit Euler, integer angle
    // integration), refresh the broad-phase, then resolve contacts (fixed-point narrow-phase +
    // iterative velocity impulses with restitution and Coulomb friction + positional correction) in
    // deterministic entity-id order.
    const char* step(kernel::World& world, simmath::Fixed dt);

    [[nodiscard]] const PhysicsConfig& config() const noexcept;

    // Number of bodies tracked by the broad-phase index (as of the last add_body/remove_body/step).
    [[nodiscard]] std::size_t body_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether `e` carries the full physics component set.
[[nodiscard]] bool is_body(const kernel::World& world, kernel::Entity e);

// Snapshot `e`'s fixed-point body state into `out`. False if `e` is dead or not a physics body.
[[nodiscard]] bool read_body(const kernel::World& world, kernel::Entity e, BodyState& out);

} // namespace context::packages::physics2d
