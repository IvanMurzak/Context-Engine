// The R-SIM-001 PRESENTATION-OBSERVER invariant (M6 P5, R-QA-013 — the DoD's cosmetic proof): the
// tooling / geometry curve DISPLAY is OFF the deterministic sim path. This test proves, against a real
// spline-active world, that arbitrary cosmetic display activity:
//   (1) leaves the sim state hash BYTE-IDENTICAL (cosmetic is off the hash),
//   (2) never writes sim state (follower position + distance unchanged; the observe API is const),
//   (3) registers NO sim component (the sim_components() set is unchanged),
// while still doing real work (it tessellates the curves with float math and free-runs a float shimmer
// over the observed markers — not a vacuous pass).

#include "context/packages/spline/cosmetic_curve.h"
#include "context/packages/spline/spline_world.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "path_fixture.h"

#include "spline_test.h"

#include <cstddef>
#include <cstdint>

using namespace context::packages::spline;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

int main()
{
    // --- build a real spline-active world (two followers moving along the XZ-plane Catmull path) ---
    kernel::World world;
    SplineWorld sw;
    CHECK(sw.set_paths(splinetest::make_paths()) == nullptr);
    const kernel::Entity a = world.create();
    const kernel::Entity b = world.create();
    CHECK(sw.add_follower(world, a, 0, sm::Fixed::from_int(1), false) == nullptr);
    CHECK(sw.add_follower(world, b, 0, sm::Fixed::from_int(2), true) == nullptr);
    for (int t = 0; t < 40; ++t)
        CHECK(sw.step(world, sm::Fixed::from_ratio(1, 60)) == nullptr);

    // --- snapshot the sim invariants BEFORE any cosmetic activity ---------------------------------
    const std::uint64_t hash_before = session::hash_world(world, session::sim_components()).root;
    const std::size_t registry_before = session::sim_components().all().size();
    FollowerState fa_before;
    CHECK(read_follower(world, a, fa_before));

    // --- run arbitrary cosmetic activity over the const world -------------------------------------
    CosmeticCurveSystem cosmetic(splinetest::make_paths());
    cosmetic.tessellate();
    // Both curves tessellated into non-empty float display polylines (real work — the geometry path).
    CHECK(cosmetic.polyline_count() == 2);
    CHECK(cosmetic.polylines().front().points.size() > 0);
    CHECK(cosmetic.polylines().back().points.size() > 0);

    cosmetic.observe(world);
    // The observer READ sim state: one marker per follower.
    CHECK(cosmetic.marker_count() == 2);
    // The followers are on the XZ plane, so their observed marker y starts at 0.
    const float y0 = cosmetic.markers().front().y;

    // The cosmetic shimmer free-runs with float math and moves the markers off the plane.
    for (int i = 0; i < 16; ++i)
        cosmetic.advance(0.03F);
    const float y1 = cosmetic.markers().front().y;
    CHECK(y1 != y0); // the cosmetic (float) path evolved

    // Re-observe at the current sim state, then run more (interleaved) and clear.
    cosmetic.observe(world);
    CHECK(cosmetic.marker_count() == 2);
    for (int i = 0; i < 8; ++i)
        cosmetic.advance(0.05F);
    cosmetic.clear();
    CHECK(cosmetic.marker_count() == 0);   // clear() releases everything (bounded, no leak)
    CHECK(cosmetic.polyline_count() == 0);

    // --- (1) the sim state hash is UNCHANGED by all that cosmetic activity ------------------------
    const std::uint64_t hash_after = session::hash_world(world, session::sim_components()).root;
    CHECK(hash_after == hash_before);

    // --- (2) sim state was never written: the follower position + distance are identical ----------
    FollowerState fa_after;
    CHECK(read_follower(world, a, fa_after));
    CHECK(fa_after.position == fa_before.position);
    CHECK(fa_after.distance == fa_before.distance);
    CHECK(fa_after.heading == fa_before.heading);

    // --- (3) the cosmetic path registered NO sim component ----------------------------------------
    CHECK(session::sim_components().all().size() == registry_before);
    CHECK(session::sim_components().by_name("spline_cosmetic") == nullptr);
    CHECK(session::sim_components().by_name("spline_marker") == nullptr);

    SPLINE_TEST_MAIN_END();
}
