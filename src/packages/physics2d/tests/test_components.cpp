// physics2d sim-component registration + state-hash folding (M6 P2, R-QA-005 / R-QA-013).
// Proves the package side of the M6-F0b seam: the four physics2d components are PODs of int64 fields
// only (the integer-only sim law), register into the combined sim_components() registry by STABLE
// NAME (never the first-touch ComponentId), leave the pristine builtin set untouched, and fold into
// the hierarchical state hash by name — so a physics-active world's digest is portable across the
// L-54 determinism matrix.

#include "context/packages/physics2d/components.h"
#include "context/packages/physics2d/physics_world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "physics2d_test.h"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace context::packages::physics2d;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

namespace
{
// --- the POD int64 layout law (sim_component.h: a sim component is exactly N contiguous int64s) ----
static_assert(sizeof(Transform2d) == 3 * sizeof(std::int64_t));
static_assert(sizeof(Velocity2d) == 3 * sizeof(std::int64_t));
static_assert(sizeof(Body2d) == 5 * sizeof(std::int64_t));
static_assert(sizeof(Collider2d) == 3 * sizeof(std::int64_t));

// A minimal one-circle world built through the public API.
kernel::Entity add_unit_circle(kernel::World& w, PhysicsWorld2d& phys, sm::Vec2 at)
{
    const kernel::Entity e = w.create();
    BodyDesc desc;
    desc.position = at;
    CHECK(phys.add_body(w, e, desc) == nullptr);
    return e;
}
} // namespace

int main()
{
    // --- registration by stable name (idempotent; pristine builtin set untouched) ---------------
    register_sim_components();

    const session::SimComponentType* transform =
        session::sim_components().by_name(kTransformComponentName);
    CHECK(transform != nullptr);
    CHECK(transform->fields.size() == 3);
    CHECK(transform->fields[0] == "px");
    CHECK(transform->fields[1] == "py");
    CHECK(transform->fields[2] == "angle");

    const session::SimComponentType* velocity =
        session::sim_components().by_name(kVelocityComponentName);
    CHECK(velocity != nullptr);
    CHECK(velocity->fields.size() == 3);
    CHECK(velocity->fields[0] == "vx");
    CHECK(velocity->fields[2] == "w");

    const session::SimComponentType* body = session::sim_components().by_name(kBodyComponentName);
    CHECK(body != nullptr);
    CHECK(body->fields.size() == 5);
    CHECK(body->fields[0] == "inv_mass");
    CHECK(body->fields[4] == "flags");

    const session::SimComponentType* collider =
        session::sim_components().by_name(kColliderComponentName);
    CHECK(collider != nullptr);
    CHECK(collider->fields.size() == 3);
    CHECK(collider->fields[0] == "shape");

    // The pristine built-in-only registry is NOT mutated by package registration (the L-54 golden
    // gate over built-ins stays green).
    CHECK(session::builtin_components().by_name(kTransformComponentName) == nullptr);
    CHECK(session::builtin_components().by_name(kBodyComponentName) == nullptr);

    // Idempotency: re-registering keeps ONE entry per component.
    {
        const std::size_t before = session::sim_components().all().size();
        register_sim_components();
        CHECK(session::sim_components().all().size() == before);
    }

    // --- a physics body folds into the hierarchical hash by stable name -------------------------
    {
        kernel::World w;
        PhysicsWorld2d phys;
        add_unit_circle(w, phys, {sm::Fixed::from_int(2), sm::Fixed::from_int(3)});

        const session::StateHash h = session::hash_world(w, session::sim_components());
        CHECK(h.archetypes.size() == 1);
        // Signature is stable component names, '+'-joined in NAME order.
        CHECK(h.archetypes[0].signature ==
              "physics2d_body+physics2d_collider+physics2d_transform+physics2d_velocity");
        CHECK(h.archetypes[0].entity_count == 1);

        // Field sensitivity: a different position produces a different root (the field is folded).
        kernel::World w2;
        PhysicsWorld2d phys2;
        add_unit_circle(w2, phys2, {sm::Fixed::from_int(2), sm::Fixed::from_int(4)});
        CHECK(session::hash_world(w2, session::sim_components()).root != h.root);

        // Determinism: an identical world hashes identically.
        kernel::World w3;
        PhysicsWorld2d phys3;
        add_unit_circle(w3, phys3, {sm::Fixed::from_int(2), sm::Fixed::from_int(3)});
        CHECK(session::hash_world(w3, session::sim_components()).root == h.root);
    }

    // --- add_body writes the described fixed-point state (default angle = 0) ---------------------
    {
        kernel::World w;
        PhysicsWorld2d phys;
        const kernel::Entity e = add_unit_circle(w, phys, {sm::kOne, sm::Fixed::from_int(2)});
        CHECK(is_body(w, e));

        const Transform2d* t = w.get<Transform2d>(e);
        CHECK(t != nullptr);
        CHECK(t->px == sm::kFixedOneRaw);
        CHECK(t->py == 2 * sm::kFixedOneRaw);
        CHECK(t->angle == 0);

        const Body2d* b = w.get<Body2d>(e);
        CHECK(b != nullptr);
        CHECK(b->flags == kBodyFlagDynamic);
        CHECK(b->inv_mass == sm::kFixedOneRaw); // mass 1 -> inv 1
        CHECK(b->inv_inertia > 0);

        const Collider2d* c = w.get<Collider2d>(e);
        CHECK(c != nullptr);
        CHECK(c->shape == kShapeCircle);
        CHECK(c->ex == sm::kFixedOneRaw);
        CHECK(c->ey == 0);

        BodyState state;
        CHECK(read_body(w, e, state));
        CHECK(state.position.y == sm::Fixed::from_int(2));
        CHECK(!state.is_static);
        CHECK(phys.body_count() == 1);
    }

    PHYSICS2D_TEST_MAIN_END();
}
