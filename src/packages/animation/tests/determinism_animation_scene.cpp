// The M6 P3 animation DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, R-SYS-002 / R-SYS-008) — the
// sim-path counterpart of the physics / particle determinism scenes.
//
// An animation-active scene — several animators over a shared locomotion rig, with distinct anim-graph
// control parameters (and a fixed mid-run parameter schedule) that drive state transitions +
// cross-fade blending + accumulated root motion — stepped N fixed ticks through the REAL
// `context_animation` package, with every tick's HIERARCHICAL canonical state hash (hash_world over the
// combined sim_components() registry — the same fold the headless session uses) accumulated into a
// trace and asserted against a cross-platform GOLDEN. Because the sim state is integer/fixed-point end
// to end (Q16, deterministic fixed trig for the yaw rotation) and the hash folds fixed-width big-endian
// integers, the goldens are PORTABLE — if any matrix platform computes a different trajectory OR a
// different fold, THAT leg goes red.
//
// Registered as the `determinism-animation-scene` ctest, joining the `determinism-*` family the
// blocking CI "Determinism gate" step (-R "^determinism-") runs on all three build-matrix legs, and the
// strict-FP `deterministic` job (its target is on that job's hand-maintained --target list).
//
// Updating the goldens: they change only when the scene or the stepper changes ON PURPOSE. Re-derive by
// running this gate — it prints the observed values — then paste them below.

#include "context/packages/animation/animation_world.h"
#include "context/runtime/session/hash.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "rig_fixture.h"

#include "animation_test.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace context::packages::animation;
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
    AnimationWorld anim;
    std::vector<kernel::Entity> actors;
};

// Three animators over the shared rig, seeded with distinct control parameters (idle / walk / turn).
void build_scene(Scene& s)
{
    CHECK(s.anim.set_rig(animationtest::make_rig()) == nullptr);
    const Fixed params[3] = {Fixed::from_int(3), Fixed::from_int(6), sm::kZero};
    for (int i = 0; i < 3; ++i)
    {
        const kernel::Entity e = s.world.create();
        CHECK(s.anim.add_animator(s.world, e) == nullptr);
        CHECK(s.anim.set_param(s.world, e, params[i]) == nullptr);
        s.actors.push_back(e);
    }
}

struct Result
{
    std::uint64_t final_root = 0;
    std::uint64_t trace_fold = 0;
    std::int64_t total_forward = 0; // sum of |pz| over actors (proves the scene really moved)
};

// Step the fixed scene kTicks times, folding each tick's hierarchical root into the trace (so a
// mid-run divergence that self-heals by the last tick still fails). A fixed mid-run parameter schedule
// forces state transitions so the golden exercises the graph + cross-fade, not just steady playback.
[[nodiscard]] Result run_fixture()
{
    Scene s;
    build_scene(s);
    session::Fnv1a trace;
    for (int t = 0; t < kTicks; ++t)
    {
        if (t == kTicks / 3)
            CHECK(s.anim.set_param(s.world, s.actors[0], Fixed::from_int(6)) == nullptr); // walk->turn
        if (t == 2 * kTicks / 3)
            CHECK(s.anim.set_param(s.world, s.actors[2], Fixed::from_int(3)) == nullptr); // idle->walk
        CHECK(s.anim.step(s.world, kDt) == nullptr);
        const session::StateHash h = session::hash_world(s.world, session::sim_components());
        trace.update_u64(h.root);
    }
    const session::StateHash final_h = session::hash_world(s.world, session::sim_components());
    Result r;
    r.final_root = final_h.root;
    r.trace_fold = trace.digest();
    for (kernel::Entity e : s.actors)
    {
        RootMotionState rm;
        CHECK(read_root_motion(s.world, e, rm));
        r.total_forward += rm.position.z.raw < 0 ? -rm.position.z.raw : rm.position.z.raw;
    }
    return r;
}

// The golden digests, derived on the reference build and asserted identical on every matrix platform
// (Linux-x64 / Win-x64 / macOS-ARM64).
constexpr std::uint64_t kGoldenFinalRoot = 0xD917294CE72B1C78ULL;
constexpr std::uint64_t kGoldenTraceFold = 0x5C7D854FC03A72FCULL;
} // namespace

int main()
{
    const Result a = run_fixture();

    // Print the observed digests so a deliberate golden update (or a platform mismatch in CI) is
    // legible directly from the test log.
    std::printf("[determinism-animation] ticks=%d forward=%lld finalRoot=0x%016llX "
                "traceFold=0x%016llX\n",
                kTicks, static_cast<long long>(a.total_forward),
                static_cast<unsigned long long>(a.final_root),
                static_cast<unsigned long long>(a.trace_fold));

    // --- within-run determinism: a second identical run reproduces the digests exactly -----------
    const Result b = run_fixture();
    CHECK(b.final_root == a.final_root);
    CHECK(b.trace_fold == a.trace_fold);

    // --- the scene really is animation-active: the actors accumulated forward root motion ----------
    CHECK(a.total_forward > 0);

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 ------
    CHECK(a.final_root == kGoldenFinalRoot);
    CHECK(a.trace_fold == kGoldenTraceFold);

    ANIMATION_TEST_MAIN_END();
}
