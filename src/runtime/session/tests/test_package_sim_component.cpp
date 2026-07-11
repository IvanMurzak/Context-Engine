// Package-contributed sim components fold into the hierarchical state hash by stable name (M6-F0b,
// R-QA-005 / R-QA-013). Proves the SimComponentRegistry seam: a package registers its own integer-only
// sim components (via register_package_sim_component / SimComponentRegistrar), they join sim_components()
// (NOT the pristine builtin_components()), and hash_world folds them by stable name across the platform
// matrix — while a world that uses only built-ins still hashes identically (the L-54 golden gate stays
// green).

#include "context/kernel/world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "session_test.h"

#include <cstdint>

using namespace context::runtime::session;
namespace kernel = context::kernel;

namespace
{
// A package-style sim component: a POD of int64 fields only (the integer-only sim law). Stands in for
// a real physics/animation package's component.
struct PkgSpin
{
    std::int64_t angle = 0;
    std::int64_t rate = 0;
};

// A second package component registered at STATIC-INIT via the registrar seam (before main runs).
struct PkgTag
{
    std::int64_t value = 0;
};
const SimComponentRegistrar<PkgTag> g_pkg_tag_registrar{"pkg_tag", {"value"}};
} // namespace

int main()
{
    // --- before registration: the package component is in NEITHER set --------------------------
    CHECK(builtin_components().by_name("pkg_spin") == nullptr);
    CHECK(sim_components().by_name("pkg_spin") == nullptr);

    // --- the static-init registrar already contributed pkg_tag to sim_components() -------------
    CHECK(sim_components().by_name("pkg_tag") != nullptr);
    CHECK(builtin_components().by_name("pkg_tag") == nullptr); // pristine set untouched

    // --- register the package component -------------------------------------------------------
    register_package_sim_component<PkgSpin>("pkg_spin", {"angle", "rate"});

    const SimComponentType* spin = sim_components().by_name("pkg_spin");
    CHECK(spin != nullptr);
    CHECK(spin->fields.size() == 2);
    CHECK(spin->fields[0] == "angle");
    CHECK(spin->fields[1] == "rate");
    // The built-ins are still present in the combined set.
    CHECK(sim_components().by_name("position") != nullptr);
    CHECK(sim_components().by_name("input_state") != nullptr);
    // The pristine built-in-only set is NOT mutated by package registration.
    CHECK(builtin_components().by_name("pkg_spin") == nullptr);

    // --- idempotency: re-registering the same type keeps ONE entry ----------------------------
    {
        const std::size_t before = sim_components().all().size();
        register_package_sim_component<PkgSpin>("pkg_spin", {"angle", "rate"});
        CHECK(sim_components().all().size() == before);
    }

    // --- a package component folds into the hierarchical hash by stable name -------------------
    {
        kernel::World w;
        const kernel::Entity e = w.create();
        w.add<Position>(e, Position{2, 3});
        w.add<PkgSpin>(e, PkgSpin{45, 7});

        const StateHash h = hash_world(w, sim_components());
        CHECK(h.archetypes.size() == 1);
        // Signature is stable component names, '+'-joined in NAME order: pkg_spin < position.
        CHECK(h.archetypes[0].signature == "pkg_spin+position");

        // Changing a PkgSpin field changes the root hash (the field is actually folded in).
        kernel::World w2;
        const kernel::Entity e2 = w2.create();
        w2.add<Position>(e2, Position{2, 3});
        w2.add<PkgSpin>(e2, PkgSpin{46, 7}); // angle differs
        CHECK(hash_world(w2, sim_components()).root != h.root);

        // Determinism: an identical world hashes identically.
        kernel::World w3;
        const kernel::Entity e3 = w3.create();
        w3.add<Position>(e3, Position{2, 3});
        w3.add<PkgSpin>(e3, PkgSpin{45, 7});
        CHECK(hash_world(w3, sim_components()).root == h.root);

        // Registration is WHAT makes it fold by name: hashing the same world through the built-in-only
        // registry (pkg_spin unregistered) yields a different signature (the "?<id>" marker) + root.
        const StateHash builtin_h = hash_world(w, builtin_components());
        CHECK(builtin_h.archetypes[0].signature != "pkg_spin+position");
        CHECK(builtin_h.root != h.root);
    }

    // --- a package component moves the SESSION hash too (registry_ points at sim_components()) --
    // (Constructing a Session picks up sim_components(); an empty session over only built-ins is
    // unaffected by pkg registration — covered by the L-54 determinism gate staying green.)

    SESSION_TEST_MAIN_END();
}
