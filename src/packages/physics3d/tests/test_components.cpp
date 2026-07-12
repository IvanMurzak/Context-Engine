// physics3d sim-component registration + state-hash folding (M6 P1, R-QA-005 / R-QA-013).
// Proves the package side of the M6-F0b seam: the four physics3d components are PODs of int64 fields
// only (the integer-only sim law), register into the combined sim_components() registry by STABLE
// NAME (never the first-touch ComponentId), leave the pristine builtin set untouched, and fold into
// the hierarchical state hash by name — so a physics-active world's digest is portable across the
// L-54 determinism matrix.

#include "context/packages/physics3d/components.h"
#include "context/packages/physics3d/physics_world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "physics3d_test.h"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace context::packages::physics3d;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

namespace
{
// --- the POD int64 layout law (sim_component.h: a sim component is exactly N contiguous int64s) ----
static_assert(sizeof(Transform3d) == 7 * sizeof(std::int64_t));
static_assert(sizeof(Velocity3d) == 6 * sizeof(std::int64_t));
static_assert(sizeof(Body3d) == 5 * sizeof(std::int64_t));
static_assert(sizeof(Collider3d) == 4 * sizeof(std::int64_t));

// A minimal one-sphere world built through the public API.
kernel::Entity add_unit_sphere(kernel::World& w, PhysicsWorld3d& phys, sm::Vec3 at)
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
    CHECK(transform->fields.size() == 7);
    CHECK(transform->fields[0] == "px");
    CHECK(transform->fields[3] == "qx");
    CHECK(transform->fields[6] == "qw");

    const session::SimComponentType* velocity =
        session::sim_components().by_name(kVelocityComponentName);
    CHECK(velocity != nullptr);
    CHECK(velocity->fields.size() == 6);
    CHECK(velocity->fields[0] == "vx");
    CHECK(velocity->fields[3] == "wx");

    const session::SimComponentType* body = session::sim_components().by_name(kBodyComponentName);
    CHECK(body != nullptr);
    CHECK(body->fields.size() == 5);
    CHECK(body->fields[0] == "inv_mass");
    CHECK(body->fields[4] == "flags");

    const session::SimComponentType* collider =
        session::sim_components().by_name(kColliderComponentName);
    CHECK(collider != nullptr);
    CHECK(collider->fields.size() == 4);
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
        PhysicsWorld3d phys;
        add_unit_sphere(w, phys, {sm::Fixed::from_int(2), sm::Fixed::from_int(3), sm::kZero});

        const session::StateHash h = session::hash_world(w, session::sim_components());
        CHECK(h.archetypes.size() == 1);
        // Signature is stable component names, '+'-joined in NAME order.
        CHECK(h.archetypes[0].signature ==
              "physics3d_body+physics3d_collider+physics3d_transform+physics3d_velocity");
        CHECK(h.archetypes[0].entity_count == 1);

        // Field sensitivity: a different position produces a different root (the field is folded).
        kernel::World w2;
        PhysicsWorld3d phys2;
        add_unit_sphere(w2, phys2, {sm::Fixed::from_int(2), sm::Fixed::from_int(4), sm::kZero});
        CHECK(session::hash_world(w2, session::sim_components()).root != h.root);

        // Determinism: an identical world hashes identically.
        kernel::World w3;
        PhysicsWorld3d phys3;
        add_unit_sphere(w3, phys3, {sm::Fixed::from_int(2), sm::Fixed::from_int(3), sm::kZero});
        CHECK(session::hash_world(w3, session::sim_components()).root == h.root);
    }

    // --- add_body writes the described fixed-point state (default orientation = identity) --------
    {
        kernel::World w;
        PhysicsWorld3d phys;
        const kernel::Entity e =
            add_unit_sphere(w, phys, {sm::kOne, sm::Fixed::from_int(2), sm::kZero});
        CHECK(is_body(w, e));

        const Transform3d* t = w.get<Transform3d>(e);
        CHECK(t != nullptr);
        CHECK(t->px == sm::kFixedOneRaw);
        CHECK(t->py == 2 * sm::kFixedOneRaw);
        CHECK(t->pz == 0);
        CHECK(t->qx == 0);
        CHECK(t->qw == sm::kFixedOneRaw); // identity

        const Body3d* b = w.get<Body3d>(e);
        CHECK(b != nullptr);
        CHECK(b->flags == kBodyFlagDynamic);
        CHECK(b->inv_mass == sm::kFixedOneRaw); // mass 1 -> inv 1
        CHECK(b->inv_inertia > 0);

        const Collider3d* c = w.get<Collider3d>(e);
        CHECK(c != nullptr);
        CHECK(c->shape == kShapeSphere);
        CHECK(c->ex == sm::kFixedOneRaw);

        BodyState state;
        CHECK(read_body(w, e, state));
        CHECK(state.position.y == sm::Fixed::from_int(2));
        CHECK(!state.is_static);
        CHECK(phys.body_count() == 1);
    }

    PHYSICS3D_TEST_MAIN_END();
}
