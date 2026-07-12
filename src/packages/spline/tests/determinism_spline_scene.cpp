// The M6 P5 spline DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, R-SYS-004) — the sim-path
// counterpart of the physics / particle / animation determinism scenes.
//
// A spline-active scene — several followers advancing along a shared multi-path set (an XZ-plane
// Catmull-Rom + a Y-rising Bezier), with distinct speeds, a mix of looping/clamping, and a fixed
// mid-run speed schedule — stepped N fixed ticks through the REAL `context_spline` package, with every
// tick's HIERARCHICAL canonical state hash (hash_world over the combined sim_components() registry —
// the same fold the headless session uses) accumulated into a trace and asserted against a
// cross-platform GOLDEN. Because the sim state is integer/fixed-point end to end (Q16 arc-length
// advance, de Casteljau / Catmull-Rom fixed cubics, deterministic fixed_sqrt arc length, deterministic
// fixed_atan2 heading) and the hash folds fixed-width big-endian integers, the goldens are PORTABLE —
// if any matrix platform computes a different trajectory OR a different fold, THAT leg goes red.
//
// Registered as the `determinism-spline-scene` ctest, joining the `determinism-*` family the blocking
// CI "Determinism gate" step (-R "^determinism-") runs on all three build-matrix legs, and the strict-FP
// `deterministic` job (its target is on that job's hand-maintained --target list).
//
// Updating the goldens: they change only when the scene or the stepper changes ON PURPOSE. Re-derive by
// running this gate — it prints the observed values — then paste them below.

#include "context/packages/spline/spline_world.h"
#include "context/runtime/session/hash.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "path_fixture.h"

#include "spline_test.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace context::packages::spline;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

using sm::Fixed;

namespace
{
constexpr int kTicks = 180;
const Fixed kDt = Fixed::from_ratio(1, 60);

struct Scene
{
    kernel::World world;
    SplineWorld spline;
    std::vector<kernel::Entity> actors;
};

// Three followers over the shared path set, seeded with distinct paths / speeds / loop modes.
void build_scene(Scene& s)
{
    CHECK(s.spline.set_paths(splinetest::make_paths()) == nullptr);
    const kernel::Entity a = s.world.create(); // Catmull path, clamping
    const kernel::Entity b = s.world.create(); // Catmull path, looping, faster
    const kernel::Entity c = s.world.create(); // Bezier path, looping
    CHECK(s.spline.add_follower(s.world, a, 0, sm::Fixed::from_int(1), false) == nullptr);
    CHECK(s.spline.add_follower(s.world, b, 0, sm::Fixed::from_int(3), true) == nullptr);
    CHECK(s.spline.add_follower(s.world, c, 1, sm::Fixed::from_ratio(3, 2), true) == nullptr);
    s.actors = {a, b, c};
}

struct Result
{
    std::uint64_t final_root = 0;
    std::uint64_t trace_fold = 0;
    std::int64_t total_forward = 0; // sum of |distance| over actors (proves the scene really moved)
};

// Step the fixed scene kTicks times, folding each tick's hierarchical root into the trace (so a mid-run
// divergence that self-heals by the last tick still fails). A fixed mid-run speed schedule exercises
// set_speed so the golden covers a live parameter change, not just steady advance.
[[nodiscard]] Result run_fixture()
{
    Scene s;
    build_scene(s);
    session::Fnv1a trace;
    for (int t = 0; t < kTicks; ++t)
    {
        if (t == kTicks / 3)
            CHECK(s.spline.set_speed(s.world, s.actors[0], sm::Fixed::from_int(2)) == nullptr);
        if (t == 2 * kTicks / 3)
            CHECK(s.spline.set_speed(s.world, s.actors[2], sm::Fixed::from_int(3)) == nullptr);
        CHECK(s.spline.step(s.world, kDt) == nullptr);
        const session::StateHash h = session::hash_world(s.world, session::sim_components());
        trace.update_u64(h.root);
    }
    const session::StateHash final_h = session::hash_world(s.world, session::sim_components());
    Result r;
    r.final_root = final_h.root;
    r.trace_fold = trace.digest();
    for (kernel::Entity e : s.actors)
    {
        FollowerState fs;
        CHECK(read_follower(s.world, e, fs));
        r.total_forward += fs.distance.raw < 0 ? -fs.distance.raw : fs.distance.raw;
    }
    return r;
}

// The golden digests, derived on the reference build and asserted identical on every matrix platform
// (Linux-x64 / Win-x64 / macOS-ARM64).
constexpr std::uint64_t kGoldenFinalRoot = 0x24177C4C65F57FEAULL;
constexpr std::uint64_t kGoldenTraceFold = 0x635CE4AED98508EDULL;
} // namespace

int main()
{
    const Result a = run_fixture();

    // Print the observed digests so a deliberate golden update (or a platform mismatch in CI) is
    // legible directly from the test log.
    std::printf("[determinism-spline] ticks=%d forward=%lld finalRoot=0x%016llX traceFold=0x%016llX\n",
                kTicks, static_cast<long long>(a.total_forward),
                static_cast<unsigned long long>(a.final_root),
                static_cast<unsigned long long>(a.trace_fold));

    // --- within-run determinism: a second identical run reproduces the digests exactly -----------
    const Result b = run_fixture();
    CHECK(b.final_root == a.final_root);
    CHECK(b.trace_fold == a.trace_fold);

    // --- the scene really is spline-active: the actors accumulated forward arc-length distance -----
    CHECK(a.total_forward > 0);

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 ------
    CHECK(a.final_root == kGoldenFinalRoot);
    CHECK(a.trace_fold == kGoldenTraceFold);

    SPLINE_TEST_MAIN_END();
}
