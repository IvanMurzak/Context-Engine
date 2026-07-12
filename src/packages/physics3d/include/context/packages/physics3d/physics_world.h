// PhysicsWorld3d — the deterministic fixed-point rigid-body 3D physics stepper (M6 P1, R-SYS-001,
// implementing the F0a physics-core decision: docs/physics-determinism-decision.md).
//
// Simulation state is integer / fixed-point ONLY (the components.h PODs — Q16 int64 fields), and
// every sim-affecting operation below is simmath integer arithmetic (add / mul / shift / compare,
// deterministic transcendental-free quaternion integration, fixed_sqrt) — no float, no platform
// libm, no FMA — so a physics-active world's hierarchical state hash (R-QA-005 / L-54) is
// byte-identical on Linux-x64 / Win-x64 / macOS-ARM64. Decoupled from render (R-SYS-001): the
// package composes on the kernel World like any other package (L-60 — the kernel never links back)
// and touches no render/presentation state.
//
// Broad-phase: the shared `context_spatial` dynamic AABB tree prunes candidate pairs. The spatial
// index is float-based; determinism is preserved STRUCTURALLY: fixed-point bounds are inflated by a
// conservative margin (strictly larger than the worst int64->float conversion error over the sim
// envelope) before conversion, so the float candidate set is a SUPERSET of the exact fixed-point
// overlap set on every platform, candidate pairs are re-decided by the EXACT fixed-point narrow
// phase, and pairs are processed in a sorted, entity-id-deterministic order (never spatial query
// order). The float prune can therefore only ever discard pairs the fixed-point narrow phase would
// also reject — it never decides sim state.
//
// v1 narrow-phase scope: sphere-sphere and sphere-box contacts (a box may be static or dynamic).
// Box-box pairs are broad-phase detected but produce no contact in v1 (documented — the DoD scene is
// dynamic spheres + box ramps on a static box floor). Inertia is a uniform scalar (see components.h).

#pragma once

#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/packages/physics3d/components.h"
#include "context/packages/physics3d/errors.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/vec.h"

#include <cstddef>
#include <memory>

namespace context::packages::physics3d
{

// Register the physics3d sim components into the combined sim_components() registry by stable name
// (sim_component.h, M6-F0b) so a physics-active world folds into the hierarchical state hash
// portably. Idempotent (re-registration overwrites); called automatically by PhysicsWorld3d's
// constructor and add_body, so any world built through this API hashes by name.
void register_sim_components();

// The collider shape of a body description.
enum class Shape : std::int64_t
{
    Sphere = kShapeSphere,
    Box = kShapeBox,
};

// A body description consumed by PhysicsWorld3d::add_body. All quantities are simmath fixed-point.
struct BodyDesc
{
    simmath::Vec3 position{};
    simmath::Quat orientation = simmath::quat_identity();
    simmath::Vec3 velocity{};
    simmath::Vec3 angular_velocity{};
    simmath::Fixed mass = simmath::kOne; // dynamic bodies: must be > 0 (kInvalidMassCode otherwise)
    simmath::Fixed restitution{};        // clamped into [0, 1]
    simmath::Fixed friction{};           // clamped into [0, 1]
    bool is_static = false;              // static bodies never move (infinite mass)
    Shape shape = Shape::Sphere;
    // Sphere: x is the radius (y/z ignored). Box: the half-extents in the body frame. Every used
    // extent must be > 0 (kInvalidShapeCode otherwise).
    simmath::Vec3 half_extents = {simmath::kOne, simmath::kOne, simmath::kOne};
};

// Global simulation configuration. Fixed-point end to end.
struct PhysicsConfig
{
    simmath::Vec3 gravity = {simmath::kZero, simmath::Fixed::from_int(-10), simmath::kZero};
    simmath::Fixed linear_damping{}; // per-second velocity damping in [0, 1); 0 == none
    int solver_iterations = 8;       // velocity-impulse iterations per step
};

// A read-back snapshot of one body's fixed-point state (read_body below).
struct BodyState
{
    simmath::Vec3 position{};
    simmath::Quat orientation = simmath::quat_identity();
    simmath::Vec3 velocity{};
    simmath::Vec3 angular_velocity{};
    bool is_static = false;
};

// The physics stepper. Owns the broad-phase index; move-only (like the kernel World / SpatialIndex).
// Every method that can fail returns nullptr on success or one of the errors.h code strings — the
// same strings the contract error catalog registers in the physics3d.* block.
class PhysicsWorld3d
{
public:
    explicit PhysicsWorld3d(const PhysicsConfig& config = {});
    ~PhysicsWorld3d();

    PhysicsWorld3d(PhysicsWorld3d&&) noexcept;
    PhysicsWorld3d& operator=(PhysicsWorld3d&&) noexcept;
    PhysicsWorld3d(const PhysicsWorld3d&) = delete;
    PhysicsWorld3d& operator=(const PhysicsWorld3d&) = delete;

    // Attach the full physics component set (Transform3d + Velocity3d + Body3d + Collider3d) to `e`
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
    const char* apply_impulse(kernel::World& world, kernel::Entity e, simmath::Vec3 impulse);

    // Advance every physics body in `world` by one fixed tick of `dt` (must be > 0, else
    // kInvalidStepCode): integrate gravity + damping + velocities (semi-implicit Euler, fixed-point
    // quaternion integration), refresh the broad-phase, then resolve contacts (fixed-point
    // narrow-phase + iterative velocity impulses with restitution and Coulomb friction + positional
    // correction) in deterministic entity-id order.
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

} // namespace context::packages::physics3d
