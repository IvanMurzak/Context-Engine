// The M6 P4 particle DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, R-SYS-003) — the sim-path
// counterpart of the physics determinism scenes.
//
// A particle-active scene — several emitters with distinct positions, base velocities, spreads,
// lifetimes, and RNG seeds — stepped N fixed ticks through the REAL `context_particles` package
// (fixed-point integration, deterministic integer-LCG emission jitter, end-of-life despawn), with
// every tick's HIERARCHICAL canonical state hash (hash_world over the combined sim_components()
// registry — the same fold the headless session uses) accumulated into a trace and asserted against a
// cross-platform GOLDEN. Because the sim state is integer/fixed-point end to end and the hash folds
// fixed-width big-endian integers, the goldens are PORTABLE — if any matrix platform computes a
// different trajectory OR a different fold, THAT leg goes red.
//
// Registered as the `determinism-particle-scene` ctest, joining the `determinism-*` family the
// blocking CI "Determinism gate" step (-R "^determinism-") runs on all three build-matrix legs, and
// the strict-FP `deterministic` job (its target is on that job's hand-maintained --target list).
//
// Updating the goldens: they change only when the scene or the stepper changes ON PURPOSE. Re-derive
// by running this gate — it prints the observed values — then paste them below.

#include "context/packages/particles/particle_world.h"
#include "context/runtime/session/hash.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "particles_test.h"

#include <cstdint>
#include <cstdio>

using namespace context::packages::particles;
namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;
using sm::Vec3;

namespace
{
constexpr int kTicks = 180;
const Fixed kDt = Fixed::from_ratio(1, 60);

struct Scene
{
    kernel::World world;
    ParticleWorld particles;
};

void add_emitter(Scene& s, Vec3 at, Vec3 vel, Fixed spread, std::int64_t rate, std::int64_t lifetime,
                 std::uint64_t seed)
{
    EmitterDesc d;
    d.position = at;
    d.velocity = vel;
    d.spread = spread;
    d.rate = rate;
    d.lifetime = lifetime;
    d.seed = seed;
    CHECK(s.particles.add_emitter(s.world, s.world.create(), d) == nullptr);
}

// The fixed scene: three distinct emitters so the run exercises constant-rate emission, per-axis
// jitter draws, gravity integration, and end-of-life despawn across staggered lifetimes.
void build_scene(Scene& s)
{
    // A fountain: fast upward jet, wide spread, short life (lots of spawn/despawn churn).
    add_emitter(s, {Fixed::from_int(-4), kZero, kZero}, {kZero, Fixed::from_int(8), kZero},
                Fixed::from_ratio(3, 2), 5, 30, 0x1111u);
    // A drifting sideways stream, medium life, small spread.
    add_emitter(s, {Fixed::from_int(2), Fixed::from_int(3), Fixed::from_int(-1)},
                {Fixed::from_int(2), kOne, kZero}, Fixed::from_ratio(1, 4), 3, 60, 0x2222u);
    // A slow single-file trickle, no spread (a pure deterministic trajectory), long life.
    add_emitter(s, {Fixed::from_int(5), Fixed::from_int(6), Fixed::from_int(2)},
                {-kOne, kZero, kOne}, kZero, 1, 90, 0x3333u);
}

struct Result
{
    std::uint64_t final_root = 0;
    std::uint64_t trace_fold = 0;
    std::int64_t total_emitted = 0;
    std::size_t final_live = 0;
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
        CHECK(s.particles.step(s.world, kDt) == nullptr);
        const session::StateHash h = session::hash_world(s.world, session::sim_components());
        trace.update_u64(h.root);
    }
    const session::StateHash final_h = session::hash_world(s.world, session::sim_components());
    Result r;
    r.final_root = final_h.root;
    r.trace_fold = trace.digest();
    r.final_live = particle_count(s.world);
    // Total emitted across every emitter (proves the scene really is particle-active).
    s.world.each<ParticleEmitter3d>(
        [&](kernel::Entity, ParticleEmitter3d& em) { r.total_emitted += em.emitted; });
    return r;
}

// The golden digests, derived on the reference build and asserted identical on every matrix platform
// (Linux-x64 / Win-x64 / macOS-ARM64).
constexpr std::uint64_t kGoldenFinalRoot = 0xFB7069ADB3AD876EULL;
constexpr std::uint64_t kGoldenTraceFold = 0x418A6954B6791B03ULL;
} // namespace

int main()
{
    const Result a = run_fixture();

    // Print the observed digests so a deliberate golden update (or a platform mismatch in CI) is
    // legible directly from the test log.
    std::printf("[determinism-particle] ticks=%d emitted=%lld live=%zu finalRoot=0x%016llX "
                "traceFold=0x%016llX\n",
                kTicks, static_cast<long long>(a.total_emitted), a.final_live,
                static_cast<unsigned long long>(a.final_root),
                static_cast<unsigned long long>(a.trace_fold));

    // --- within-run determinism: a second identical run reproduces the digests exactly -----------
    const Result b = run_fixture();
    CHECK(b.final_root == a.final_root);
    CHECK(b.trace_fold == a.trace_fold);

    // --- the scene really is particle-active: emitters produced particles that later despawned ----
    // (Guards against a silently inert scene making the golden vacuous.)
    CHECK(a.total_emitted > 0);
    CHECK(a.final_live > 0);
    CHECK(a.final_live < static_cast<std::size_t>(a.total_emitted)); // despawn actually happened

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 ------
    CHECK(a.final_root == kGoldenFinalRoot);
    CHECK(a.trace_fold == kGoldenTraceFold);

    PARTICLES_TEST_MAIN_END();
}
