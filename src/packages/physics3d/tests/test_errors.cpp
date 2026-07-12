// physics3d fail-closed refusals (M6 P1, R-QA-013 failure paths): the physics3d.* code strings the
// contract error catalog registers, asserted at their source of truth (errors.h) — an invalid body
// description or a physics op on a non-body entity is refused deterministically and leaves the world
// untouched.

#include "context/packages/physics3d/physics_world.h"
#include "physics3d_test.h"

#include <cstring>

using namespace context::packages::physics3d;
namespace kernel = context::kernel;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
[[nodiscard]] bool same_code(const char* got, const char* expected)
{
    return got != nullptr && std::strcmp(got, expected) == 0;
}
} // namespace

int main()
{
    // --- the code strings are the exact catalog identities (pins the physics3d.* block) ----------
    CHECK(std::strcmp(kInvalidEntityCode, "physics3d.invalid_entity") == 0);
    CHECK(std::strcmp(kMissingComponentCode, "physics3d.missing_component") == 0);
    CHECK(std::strcmp(kInvalidShapeCode, "physics3d.invalid_shape") == 0);
    CHECK(std::strcmp(kInvalidMassCode, "physics3d.invalid_mass") == 0);
    CHECK(std::strcmp(kInvalidStepCode, "physics3d.invalid_step") == 0);

    kernel::World w;
    PhysicsWorld3d phys;

    // --- invalid shape: non-positive sphere radius / box half-extent (world untouched) -----------
    {
        const kernel::Entity e = w.create();
        BodyDesc desc;
        desc.half_extents = {kZero, kOne, kOne}; // sphere radius 0
        CHECK(same_code(phys.add_body(w, e, desc), kInvalidShapeCode));
        CHECK(!is_body(w, e));

        desc.half_extents = {-kOne, kOne, kOne}; // negative radius
        CHECK(same_code(phys.add_body(w, e, desc), kInvalidShapeCode));

        desc.shape = Shape::Box;
        desc.half_extents = {kOne, kZero, kOne}; // box with a zero half-extent
        CHECK(same_code(phys.add_body(w, e, desc), kInvalidShapeCode));
        CHECK(!is_body(w, e));
        CHECK(phys.body_count() == 0);
    }

    // --- invalid mass: a non-positive dynamic mass is refused; a static body needs none ----------
    {
        const kernel::Entity e = w.create();
        BodyDesc desc;
        desc.mass = kZero;
        CHECK(same_code(phys.add_body(w, e, desc), kInvalidMassCode));
        CHECK(!is_body(w, e));

        desc.mass = -kOne;
        CHECK(same_code(phys.add_body(w, e, desc), kInvalidMassCode));

        desc.is_static = true; // static: infinite mass, desc.mass ignored
        CHECK(phys.add_body(w, e, desc) == nullptr);
        CHECK(is_body(w, e));
        BodyState s;
        CHECK(read_body(w, e, s));
        CHECK(s.is_static);
        CHECK(phys.remove_body(w, e) == nullptr);
        CHECK(!is_body(w, e));
        CHECK(phys.body_count() == 0);
    }

    // --- invalid entity: null and destroyed handles are refused everywhere -----------------------
    {
        const kernel::Entity null_entity{}; // generation 0 == invalid
        BodyDesc desc;
        CHECK(same_code(phys.add_body(w, null_entity, desc), kInvalidEntityCode));
        CHECK(same_code(phys.remove_body(w, null_entity), kInvalidEntityCode));
        CHECK(same_code(phys.apply_impulse(w, null_entity, {kOne, kZero, kZero}),
                        kInvalidEntityCode));

        const kernel::Entity dead = w.create();
        w.destroy(dead);
        CHECK(same_code(phys.add_body(w, dead, desc), kInvalidEntityCode));
        CHECK(same_code(phys.remove_body(w, dead), kInvalidEntityCode));
        CHECK(same_code(phys.apply_impulse(w, dead, {kOne, kZero, kZero}), kInvalidEntityCode));
    }

    // --- missing component set: physics ops on a live non-body entity ----------------------------
    {
        const kernel::Entity plain = w.create();
        CHECK(same_code(phys.remove_body(w, plain), kMissingComponentCode));
        CHECK(same_code(phys.apply_impulse(w, plain, {kOne, kZero, kZero}),
                        kMissingComponentCode));
        BodyState s;
        CHECK(!read_body(w, plain, s));
        CHECK(!is_body(w, plain));
    }

    // --- invalid step: a non-positive tick duration is refused -----------------------------------
    CHECK(same_code(phys.step(w, kZero), kInvalidStepCode));
    CHECK(same_code(phys.step(w, -kOne), kInvalidStepCode));
    CHECK(phys.step(w, Fixed::from_ratio(1, 60)) == nullptr); // a positive dt steps fine

    // --- static bodies: impulses are a deterministic no-op ---------------------------------------
    {
        const kernel::Entity e = w.create();
        BodyDesc desc;
        desc.is_static = true;
        desc.shape = Shape::Box;
        desc.half_extents = {kOne, kOne, kOne};
        CHECK(phys.add_body(w, e, desc) == nullptr);
        CHECK(phys.apply_impulse(w, e, {Fixed::from_int(100), kZero, kZero}) == nullptr);
        BodyState s;
        CHECK(read_body(w, e, s));
        CHECK(s.velocity.x.raw == 0); // unmoved: static means static

        // A dynamic body DOES take the impulse (mass 2 -> delta-v = impulse / 2).
        const kernel::Entity d = w.create();
        BodyDesc dyn;
        dyn.mass = Fixed::from_int(2);
        dyn.position = {Fixed::from_int(30), kZero, kZero}; // far from everything else
        CHECK(phys.add_body(w, d, dyn) == nullptr);
        CHECK(phys.apply_impulse(w, d, {Fixed::from_int(4), kZero, kZero}) == nullptr);
        CHECK(read_body(w, d, s));
        CHECK(s.velocity.x == Fixed::from_int(2));
    }

    PHYSICS3D_TEST_MAIN_END();
}
