// The M6 P1 physics-active DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, issue #174) — the
// REAL-scene successor of F0a's determinism-physics-wedge spike proof.
//
// F0a proved a trivial physics-SHAPED fixed-point step hashes byte-identically across the wedge
// matrix (Linux-x64 / Win-x64 / macOS-ARM64) before any physics package existed. This gate proves
// the REAL thing: a physics-active scene — dynamic spheres with distinct materials, a rotated box
// ramp, a static box floor — stepped N fixed ticks through the actual `context_physics3d` package
// (fixed-point integration, spatial broad-phase prune, fixed-point narrow phase + impulse solver),
// with every tick's HIERARCHICAL canonical state hash (hash_world over the combined sim_components()
// registry — the same fold the headless session uses) accumulated into a trace, asserted against a
// cross-platform GOLDEN. Because the sim state is integer/fixed-point end to end and the hash folds
// fixed-width big-endian integers, the goldens are PORTABLE — if any matrix platform computes a
// different trajectory OR a different fold, THAT leg goes red.
//
// Registered as the `determinism-physics3d-scene` ctest, joining the `determinism-*` family the
// blocking CI "Determinism gate" step (-R "^determinism-") runs on all three build-matrix legs.
//
// Updating the goldens: they change only when the scene or the solver changes ON PURPOSE. Re-derive
// by running this gate — it prints the observed values — then paste them below.

#include "context/packages/physics3d/physics_world.h"
#include "context/runtime/session/hash.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "physics3d_test.h"

#include <cstdint>
#include <cstdio>

using namespace context::packages::physics3d;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec3;

namespace
{
constexpr int kTicks = 150;
const Fixed kDt = Fixed::from_ratio(1, 60);

void add_sphere(kernel::World& w, PhysicsWorld3d& phys, Vec3 at, Vec3 vel, Fixed restitution,
                Fixed friction)
{
    BodyDesc desc;
    desc.position = at;
    desc.velocity = vel;
    desc.restitution = restitution;
    desc.friction = friction;
    CHECK(phys.add_body(w, w.create(), desc) == nullptr);
}

// The fixed scene: a static floor (top at y == 0), a static ramp tilted about z so downhill is +x,
// and four dynamic unit spheres with distinct materials + initial velocities, so the run exercises
// integration, sphere-ramp contact (slide + roll), sphere-floor restitution bounces, and
// sphere-sphere impulses.
struct Scene
{
    kernel::World world;
    PhysicsWorld3d phys;
};

void build_scene(Scene& s)
{
    kernel::World& w = s.world;
    PhysicsWorld3d& phys = s.phys;

    // Floor: 40x40 slab, top surface at y == 0, moderate friction.
    {
        BodyDesc floor;
        floor.position = {kZero, Fixed::from_int(-1), kZero};
        floor.is_static = true;
        floor.shape = Shape::Box;
        floor.half_extents = {Fixed::from_int(20), kOne, Fixed::from_int(20)};
        floor.friction = Fixed::from_ratio(1, 2);
        floor.restitution = Fixed::from_ratio(1, 10);
        CHECK(phys.add_body(w, w.create(), floor) == nullptr);
    }
    // Ramp: tilted about z by ~ -20 degrees (downhill toward +x), sitting above the floor.
    {
        BodyDesc ramp;
        ramp.position = {Fixed::from_int(-8), Fixed::from_int(2), kZero};
        ramp.orientation =
            sm::quat_from_axis_angle({kZero, kZero, kOne}, Fixed::from_ratio(-35, 100));
        ramp.is_static = true;
        ramp.shape = Shape::Box;
        ramp.half_extents = {Fixed::from_int(6), Fixed::from_ratio(1, 2), Fixed::from_int(4)};
        ramp.friction = Fixed::from_ratio(3, 10);
        ramp.restitution = Fixed::from_ratio(1, 10);
        CHECK(phys.add_body(w, w.create(), ramp) == nullptr);
    }
    // Four dynamic unit spheres (mass 1) with distinct drops, materials, and impulses.
    add_sphere(s.world, s.phys, {Fixed::from_int(-10), Fixed::from_int(6), kZero},
               {kZero, kZero, kZero}, Fixed::from_ratio(1, 5), Fixed::from_ratio(2, 5));
    add_sphere(s.world, s.phys, {Fixed::from_int(-6), Fixed::from_int(7), kOne},
               {kOne, kZero, kZero}, Fixed::from_ratio(1, 5), Fixed::from_ratio(1, 5));
    add_sphere(s.world, s.phys, {Fixed::from_int(4), Fixed::from_int(5), kZero},
               {-kOne, kZero, kZero}, Fixed::from_ratio(4, 5), kZero);
    add_sphere(s.world, s.phys, {Fixed::from_int(6), Fixed::from_int(3), -kOne},
               {kZero, kZero, kOne / 2}, Fixed::from_ratio(3, 5), Fixed::from_ratio(1, 2));
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
constexpr std::uint64_t kGoldenFinalRoot = 0xB7FF76AFE063CF4CULL;
constexpr std::uint64_t kGoldenTraceFold = 0x6A966098D3AEB715ULL;
} // namespace

int main()
{
    const Result a = run_fixture();

    // Print the observed digests so a deliberate golden update (or a platform mismatch in CI) is
    // legible directly from the test log.
    std::printf(
        "[determinism-physics3d] ticks=%d bodies=6 finalRoot=0x%016llX traceFold=0x%016llX\n",
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
        bool any_upward = false; // a bounce flipped some sphere's vertical velocity
        for (int t = 0; t < kTicks; ++t)
        {
            CHECK(s.phys.step(s.world, kDt) == nullptr);
            s.world.each<Transform3d, Velocity3d, Body3d, Collider3d>(
                [&](kernel::Entity, Transform3d& tr, Velocity3d& vel, Body3d& body, Collider3d&)
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

    PHYSICS3D_TEST_MAIN_END();
}
