// The M6 P2 physics-active DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, issue #176) — the 2D
// sibling of determinism-physics3d-scene.
//
// A 2D physics-active scene — dynamic circles with distinct materials + initial velocities (one a
// fast mover that pushes another), on two static box PLATFORMS (a wide floor + a tilted ramp) —
// stepped N fixed ticks through the actual `context_physics2d` package (fixed-point integration,
// spatial degenerate-Z broad-phase prune, fixed-point narrow phase + impulse solver), with every
// tick's HIERARCHICAL canonical state hash (hash_world over the combined sim_components() registry —
// the same fold the headless session uses) accumulated into a trace, asserted against a cross-platform
// GOLDEN. Because the sim state is integer/fixed-point end to end and the hash folds fixed-width
// big-endian integers, the goldens are PORTABLE — if any matrix platform computes a different
// trajectory OR a different fold, THAT leg goes red.
//
// Registered as the `determinism-physics2d-scene` ctest, joining the `determinism-*` family the
// blocking CI "Determinism gate" step (-R "^determinism-") runs on all three build-matrix legs, AND
// added to the strict-FP `deterministic` job's explicit --target list in .github/workflows/ci.yml.
//
// Updating the goldens: they change only when the scene or the solver changes ON PURPOSE. Re-derive
// by running this gate — it prints the observed values — then paste them below.

#include "context/packages/physics2d/physics_world.h"
#include "context/runtime/session/hash.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "physics2d_test.h"

#include <cstdint>
#include <cstdio>

using namespace context::packages::physics2d;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec2;

namespace
{
constexpr int kTicks = 150;
const Fixed kDt = Fixed::from_ratio(1, 60);

void add_circle(kernel::World& w, PhysicsWorld2d& phys, Vec2 at, Vec2 vel, Fixed restitution,
                Fixed friction)
{
    BodyDesc desc;
    desc.position = at;
    desc.velocity = vel;
    desc.restitution = restitution;
    desc.friction = friction;
    CHECK(phys.add_body(w, w.create(), desc) == nullptr);
}

// The fixed scene: a static floor (top at y == 0), a static ramp tilted so downhill is +x, and four
// dynamic unit circles with distinct materials + initial velocities (the 4th a fast leftward mover
// that pushes the 3rd — the DoD "pushable body"), so the run exercises integration, circle-ramp
// contact (slide), circle-floor restitution bounces, and circle-circle impulses.
struct Scene
{
    kernel::World world;
    PhysicsWorld2d phys;
};

void build_scene(Scene& s)
{
    kernel::World& w = s.world;
    PhysicsWorld2d& phys = s.phys;

    // Floor: 40-wide slab, top surface at y == 0, moderate friction (a static box PLATFORM).
    {
        BodyDesc floor;
        floor.position = {kZero, Fixed::from_int(-1)};
        floor.is_static = true;
        floor.shape = Shape::Box;
        floor.half_extents = {Fixed::from_int(20), kOne};
        floor.friction = Fixed::from_ratio(1, 2);
        floor.restitution = Fixed::from_ratio(1, 10);
        CHECK(phys.add_body(w, w.create(), floor) == nullptr);
    }
    // Ramp: tilted by ~ -20 degrees (downhill toward +x), sitting above the floor (a static box
    // PLATFORM).
    {
        BodyDesc ramp;
        ramp.position = {Fixed::from_int(-8), Fixed::from_int(2)};
        ramp.angle = Fixed::from_ratio(-35, 100);
        ramp.is_static = true;
        ramp.shape = Shape::Box;
        ramp.half_extents = {Fixed::from_int(6), Fixed::from_ratio(1, 2)};
        ramp.friction = Fixed::from_ratio(3, 10);
        ramp.restitution = Fixed::from_ratio(1, 10);
        CHECK(phys.add_body(w, w.create(), ramp) == nullptr);
    }
    // Four dynamic unit circles (mass 1) with distinct drops, materials, and impulses.
    add_circle(s.world, s.phys, {Fixed::from_int(-10), Fixed::from_int(6)}, {kZero, kZero},
               Fixed::from_ratio(1, 5), Fixed::from_ratio(2, 5));
    add_circle(s.world, s.phys, {Fixed::from_int(-6), Fixed::from_int(7)}, {kOne, kZero},
               Fixed::from_ratio(1, 5), Fixed::from_ratio(1, 5));
    add_circle(s.world, s.phys, {Fixed::from_int(4), Fixed::from_int(5)}, {-kOne, kZero},
               Fixed::from_ratio(4, 5), kZero);
    add_circle(s.world, s.phys, {Fixed::from_int(6), Fixed::from_int(3)},
               {Fixed::from_int(-2), kZero}, Fixed::from_ratio(3, 5), Fixed::from_ratio(1, 2));
}

struct Result
{
    std::uint64_t final_root = 0;
    std::uint64_t trace_fold = 0;
};

// Step the fixed scene kTicks times, folding each tick's hierarchical root into the trace (so a
// mid-run divergence that self-heals by the last tick still fails).
[[nodiscard]] Result run_fixture()
{
    Scene s;
    build_scene(s);
    session::Fnv1a trace;
    for (int t = 0; t < kTicks; ++t)
    {
        CHECK(s.phys.step(s.world, kDt) == nullptr);
        const session::StateHash h = session::hash_world(s.world, session::sim_components());
        trace.update_u64(h.root);
    }
    const session::StateHash final_h = session::hash_world(s.world, session::sim_components());
    return {final_h.root, trace.digest()};
}

// The golden digests, derived on the reference build and asserted identical on every matrix
// platform (Linux-x64 / Win-x64 / macOS-ARM64).
constexpr std::uint64_t kGoldenFinalRoot = 0xFFEC3C5CE954D75FULL;
constexpr std::uint64_t kGoldenTraceFold = 0x4E9315C0B20E6742ULL;
} // namespace

int main()
{
    const Result a = run_fixture();

    // Print the observed digests so a deliberate golden update (or a platform mismatch in CI) is
    // legible directly from the test log.
    std::printf(
        "[determinism-physics2d] ticks=%d bodies=6 finalRoot=0x%016llX traceFold=0x%016llX\n",
        kTicks, static_cast<unsigned long long>(a.final_root),
        static_cast<unsigned long long>(a.trace_fold));

    // --- within-run determinism: a second identical run reproduces the digests exactly -----------
    const Result b = run_fixture();
    CHECK(b.final_root == a.final_root);
    CHECK(b.trace_fold == a.trace_fold);

    // --- the scene really is physics-active: bodies collided and settled sanely ------------------
    // (Guards against a silently inert scene making the golden vacuous.)
    {
        Scene s;
        build_scene(s);
        bool any_upward = false; // a bounce flipped some circle's vertical velocity
        for (int t = 0; t < kTicks; ++t)
        {
            CHECK(s.phys.step(s.world, kDt) == nullptr);
            s.world.each<Transform2d, Velocity2d, Body2d, Collider2d>(
                [&](kernel::Entity, Transform2d& tr, Velocity2d& vel, Body2d& body, Collider2d&)
                {
                    if (body.flags == kBodyFlagDynamic && vel.vy > 0)
                        any_upward = true;
                    // No dynamic body ever tunnels through the floor (top at y == 0).
                    if (body.flags == kBodyFlagDynamic)
                        CHECK(tr.py > -sm::kFixedOneRaw);
                });
        }
        CHECK(any_upward);
    }

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 -----
    CHECK(a.final_root == kGoldenFinalRoot);
    CHECK(a.trace_fold == kGoldenTraceFold);

    PHYSICS2D_TEST_MAIN_END();
}
