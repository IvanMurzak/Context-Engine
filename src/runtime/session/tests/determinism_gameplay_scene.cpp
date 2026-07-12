// The M6 X3 COMBINED-GAMEPLAY DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, issue #192) — the
// composition-hardened successor of the per-package determinism scenes.
//
// Each of the five landed sim packages proved wedge-determinism INDIVIDUALLY (the
// determinism-physics3d/physics2d/particle/animation/spline/input-scene ctests). This gate proves
// they COMPOSE deterministically in ONE scene: dynamic context_physics3d bodies (spheres on a floor +
// a rotated ramp) + context_physics2d bodies (circles + a pushable dynamic box on a platform) + a
// root-motion context_animation entity + sim-path context_particles emitting + a transcendental
// "steer" gameplay system (the F0b simmath fixed_sin / fixed_cos / fixed_atan2 / fixed_sqrt on the
// sim path), stepped N fixed ticks in a FIXED system order, with every tick's HIERARCHICAL canonical
// state hash (hash_world over the combined sim_components() registry — the same fold the headless
// session uses) accumulated into a trace and asserted against a cross-platform GOLDEN. The
// composition risk surfaces this exercises are exactly where cross-platform drift hides: system
// ordering, SHARED entities touched by multiple systems (the hero is a physics3d body AND a
// root-motion animator; one particle emitter rides a physics3d body's entity), and FMA /
// transcendental contraction across the combined math. Because the sim state is integer/fixed-point
// end to end and the hash folds fixed-width big-endian integers, the goldens are PORTABLE — if any
// matrix platform (Linux-x64 / Win-x64 / macOS-ARM64) computes a different trajectory OR a different
// fold, THAT leg goes red.
//
// The gate ALSO exercises the `context determinism diff` auto-triage machinery (triage.h — the
// replay-bisect + per-system walk + canonical snapshot field-diff that command consumes) over the
// combined scene: a controlled physics divergence and a controlled animation divergence are injected
// at a KNOWN (tick, system) and each is attributed to the exact (tick, system, entity,
// componentField) through the R-LANG-010 sim-component schemas.
//
// Registered as the `determinism-gameplay-scene` ctest, joining the `determinism-*` family the
// blocking CI "Determinism gate" step (-R "^determinism-") runs on all three build-matrix legs, and
// the strict-FP `deterministic` job (its target is on that job's hand-maintained --target list).
//
// Updating the goldens: they change only when the scene or a package stepper changes ON PURPOSE.
// Re-derive by running this gate — it prints the observed values — then paste them below.

#include "context/packages/animation/animation_world.h"
#include "context/packages/particles/particle_world.h"
#include "context/packages/physics2d/physics_world.h"
#include "context/packages/physics3d/physics_world.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/trig.h"
#include "context/packages/simmath/vec.h"
#include "context/runtime/session/hash.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"
#include "context/runtime/session/triage.h"
#include "session_test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace sm = context::packages::simmath;
namespace p3 = context::packages::physics3d;
namespace p2 = context::packages::physics2d;
namespace anim = context::packages::animation;
namespace parts = context::packages::particles;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
constexpr int kTicks = 240;
const Fixed kDt = Fixed::from_ratio(1, 60);

// The FIXED system order every tick runs (the ordering axis the R-LANG-011 scheduler will own; the
// per-system hash walk below attributes a divergence to one of the 5 systems — steer, physics3d,
// physics2d, animation, particles — exactly as the session's per-system trace does for the demo
// scenario).
constexpr std::size_t kSystemCount = 5;

// The fixed mid-run anim-graph parameter schedule (this scene's "injected input"): applied at the
// START of the named tick, before any system runs — identically on every run.
constexpr int kParamTickWalkerIdle = 80;  // walker: stride -> idle
constexpr int kParamTickHeroStride = 160; // hero: idle -> stride

// --- the inline locomotion rig (2 joints, 2 clips, a 2-state parameter-gated graph) ---------------
// Sim-visible variety comes from the clips' ROOT-MOTION tracks (root_velocity + yaw_rate — the
// stride clip's non-zero yaw rate drives the package's internal fixed-trig heading rotation); the
// joint tracks feed only the cosmetic pose observer, which is OFF the sim path (R-SIM-001).
anim::Rig make_rig()
{
    anim::Rig rig;

    rig.skeleton.parents = {-1, 0};
    sm::Transform root;
    root.translation = {kZero, kZero, kZero};
    root.rotation = sm::quat_identity();
    root.scale = {kOne, kOne, kOne};
    sm::Transform spine = root;
    spine.translation = {kZero, kOne, kZero};
    rig.skeleton.bind = {root, spine};

    anim::Clip idle;
    idle.duration = kOne;
    idle.loop = true;
    idle.tracks = {{}, {}};
    idle.root_velocity = {kZero, kZero, kZero};
    idle.yaw_rate = kZero;

    anim::Clip stride;
    stride.duration = kOne;
    stride.loop = true;
    stride.tracks = {{}, {}};
    stride.root_velocity = {kZero, kZero, Fixed::from_int(2)};
    stride.yaw_rate = Fixed::from_ratio(1, 8);

    rig.clips = {idle, stride};

    anim::GraphState st_idle;
    st_idle.clip = 0;
    st_idle.transitions = {
        anim::Transition{1, anim::CompareOp::greater_equal, Fixed::from_int(5),
                         Fixed::from_ratio(1, 4)},
    };
    anim::GraphState st_stride;
    st_stride.clip = 1;
    st_stride.transitions = {
        anim::Transition{0, anim::CompareOp::less, Fixed::from_int(5), Fixed::from_ratio(1, 4)},
    };
    rig.graph.states = {st_idle, st_stride};
    rig.graph.initial = 0;

    return rig;
}

// --- the combined scene ----------------------------------------------------------------------------
struct Scene
{
    kernel::World world;
    p3::PhysicsWorld3d phys3;
    p2::PhysicsWorld2d phys2;
    anim::AnimationWorld animation;
    parts::ParticleWorld particles;

    kernel::Entity hero{};   // SHARED: physics3d dynamic sphere + root-motion animator
    kernel::Entity steer3{}; // SHARED: physics3d dynamic sphere + the fountain particle emitter
    kernel::Entity steer2{}; // physics2d dynamic circle the trig homing kicks steer
    kernel::Entity walker{}; // standalone animator (strides from tick 0, idles from tick 80)
};

void add_sphere3(Scene& s, kernel::Entity e, sm::Vec3 at, sm::Vec3 vel, Fixed restitution,
                 Fixed friction)
{
    p3::BodyDesc d;
    d.position = at;
    d.velocity = vel;
    d.restitution = restitution;
    d.friction = friction;
    CHECK(s.phys3.add_body(s.world, e, d) == nullptr);
}

void add_circle2(Scene& s, kernel::Entity e, sm::Vec2 at, sm::Vec2 vel, Fixed restitution,
                 Fixed friction)
{
    p2::BodyDesc d;
    d.position = at;
    d.velocity = vel;
    d.restitution = restitution;
    d.friction = friction;
    CHECK(s.phys2.add_body(s.world, e, d) == nullptr);
}

// The fixed scene: every landed sim package active in ONE world, with two shared-entity archetypes.
void build_scene(Scene& s)
{
    // --- physics3d: static floor (top at y == 0) + a rotated ramp + three dynamic spheres --------
    {
        p3::BodyDesc floor;
        floor.position = {kZero, Fixed::from_int(-1), kZero};
        floor.is_static = true;
        floor.shape = p3::Shape::Box;
        floor.half_extents = {Fixed::from_int(20), kOne, Fixed::from_int(20)};
        floor.friction = Fixed::from_ratio(1, 2);
        floor.restitution = Fixed::from_ratio(1, 10);
        CHECK(s.phys3.add_body(s.world, s.world.create(), floor) == nullptr);
    }
    {
        p3::BodyDesc ramp;
        ramp.position = {Fixed::from_int(-8), Fixed::from_int(2), kZero};
        ramp.orientation =
            sm::quat_from_axis_angle({kZero, kZero, kOne}, Fixed::from_ratio(-35, 100));
        ramp.is_static = true;
        ramp.shape = p3::Shape::Box;
        ramp.half_extents = {Fixed::from_int(6), Fixed::from_ratio(1, 2), Fixed::from_int(4)};
        ramp.friction = Fixed::from_ratio(3, 10);
        ramp.restitution = Fixed::from_ratio(1, 10);
        CHECK(s.phys3.add_body(s.world, s.world.create(), ramp) == nullptr);
    }
    s.hero = s.world.create();
    add_sphere3(s, s.hero, {Fixed::from_int(-10), Fixed::from_int(6), kZero}, {kZero, kZero, kZero},
                Fixed::from_ratio(1, 5), Fixed::from_ratio(2, 5));
    s.steer3 = s.world.create();
    add_sphere3(s, s.steer3, {Fixed::from_int(4), Fixed::from_int(5), kZero}, {-kOne, kZero, kZero},
                Fixed::from_ratio(3, 5), Fixed::from_ratio(1, 5));
    add_sphere3(s, s.world.create(), {Fixed::from_int(6), Fixed::from_int(3), -kOne},
                {kZero, kZero, kOne / 2}, Fixed::from_ratio(4, 5), kZero);

    // --- physics2d: static platform (top at y == 0) + two dynamic circles + a pushable box -------
    {
        p2::BodyDesc platform;
        platform.position = {kZero, Fixed::from_int(-1)};
        platform.is_static = true;
        platform.shape = p2::Shape::Box;
        platform.half_extents = {Fixed::from_int(20), kOne};
        platform.friction = Fixed::from_ratio(1, 2);
        platform.restitution = Fixed::from_ratio(1, 10);
        CHECK(s.phys2.add_body(s.world, s.world.create(), platform) == nullptr);
    }
    s.steer2 = s.world.create();
    add_circle2(s, s.steer2, {Fixed::from_int(-3), Fixed::from_int(4)}, {kOne, kZero},
                Fixed::from_ratio(3, 5), Fixed::from_ratio(1, 5));
    add_circle2(s, s.world.create(), {Fixed::from_int(3), Fixed::from_int(6)}, {-kOne, kOne},
                Fixed::from_ratio(2, 5), Fixed::from_ratio(2, 5));
    {
        p2::BodyDesc box;
        box.position = {kZero, Fixed::from_int(2)};
        box.shape = p2::Shape::Box;
        box.half_extents = {kOne, kOne / 2};
        box.mass = Fixed::from_int(2);
        box.friction = Fixed::from_ratio(1, 2);
        box.restitution = Fixed::from_ratio(1, 5);
        CHECK(s.phys2.add_body(s.world, s.world.create(), box) == nullptr);
    }

    // --- animation: the shared hero (idle until tick 160) + a standalone walker (strides first) --
    CHECK(s.animation.set_rig(make_rig()) == nullptr);
    CHECK(s.animation.add_animator(s.world, s.hero) == nullptr);
    s.walker = s.world.create();
    CHECK(s.animation.add_animator(s.world, s.walker) == nullptr);
    CHECK(s.animation.set_param(s.world, s.walker, Fixed::from_int(6)) == nullptr);

    // --- particles: a fountain riding the steer3 body's entity + a standalone trickle ------------
    {
        parts::EmitterDesc fountain;
        fountain.position = {Fixed::from_int(-4), kZero, kZero};
        fountain.velocity = {kZero, Fixed::from_int(8), kZero};
        fountain.spread = Fixed::from_ratio(3, 2);
        fountain.rate = 4;
        fountain.lifetime = 40;
        fountain.seed = 0xA11CEu;
        CHECK(s.particles.add_emitter(s.world, s.steer3, fountain) == nullptr);
    }
    {
        parts::EmitterDesc trickle;
        trickle.position = {Fixed::from_int(5), Fixed::from_int(6), Fixed::from_int(2)};
        trickle.velocity = {-kOne, kZero, kOne};
        trickle.spread = Fixed::from_ratio(1, 4);
        trickle.rate = 1;
        trickle.lifetime = 80;
        trickle.seed = 0xB0Bu;
        CHECK(s.particles.add_emitter(s.world, s.world.create(), trickle) == nullptr);
    }
}

// The transcendental "steer" gameplay system: every 8th tick it derives impulses from the F0b
// deterministic fixed trig — a swirling (cos, sin) kick scaled by fixed_sqrt for the 3D steer body,
// and a fixed_atan2 homing kick (aim back at the origin) for the 2D steer body. Trig OUTPUT feeds
// SIM state (body velocities), so any per-platform transcendental / FMA contraction difference in
// the combined math would move the hash.
void system_steer(Scene& s, int tick)
{
    if (tick % 8 != 0)
        return;
    const Fixed angle = Fixed::from_ratio(tick, 16); // spans several 2*pi wraps over the run
    const Fixed swirl = sm::fixed_sqrt(Fixed::from_int(1 + tick % 3)) / 4;
    const sm::Vec3 kick3 = {sm::fixed_cos(angle) * swirl, kZero, sm::fixed_sin(angle) * swirl};
    CHECK(s.phys3.apply_impulse(s.world, s.steer3, kick3) == nullptr);

    const p2::Transform2d* tr = s.world.get<p2::Transform2d>(s.steer2);
    CHECK(tr != nullptr);
    if (tr != nullptr)
    {
        const Fixed hx = Fixed::from_raw(tr->px);
        const Fixed hy = Fixed::from_raw(tr->py);
        const Fixed home = sm::fixed_atan2(-hy, -hx); // heading back toward the origin
        const sm::Vec2 kick2 = {sm::fixed_cos(home) / 2, sm::fixed_sin(home) / 2};
        CHECK(s.phys2.apply_impulse(s.world, s.steer2, kick2) == nullptr);
    }
}

// A controlled divergence injected right AFTER a named system ran at a known tick — modeling exactly
// what a real cross-platform divergence looks like (that system computed a different value on one
// platform). Applied on the RIGHT run only.
struct Tamper
{
    int tick = -1;
    std::size_t system = 0;
    void (*fn)(Scene&) = nullptr;
};

// A canonical world snapshot captured right after a named system ran at a known tick (the pass-2
// re-run of the triage flow — determinism guarantees the re-run reproduces the recorded run).
struct Capture
{
    int tick = -1;
    std::size_t system = 0;
    session::WorldSnapshot out;
};

struct RunTrace
{
    std::vector<std::uint64_t> roots;                             // per-tick end-of-tick root
    std::vector<std::array<std::uint64_t, kSystemCount>> systems; // per-tick post-system roots
    std::uint64_t final_root = 0;
    std::uint64_t trace_fold = 0;

    // Activity + identity read-back (proves the scene is genuinely multi-system active, and gives
    // the triage assertions the deterministic entity ids).
    bool any_upward = false;      // some dynamic 3D sphere bounced (vertical velocity flipped up)
    std::int64_t hero_forward = 0; // |accumulated root-motion pz| of the hero (raw Q16)
    std::int64_t total_emitted = 0;
    std::size_t final_live = 0;
    std::int64_t p2_travel = 0; // |steer2 displacement from spawn| (raw Q16, x + y)
    kernel::Entity hero{};
    kernel::Entity steer3{};
    kernel::Entity steer2{};
};

// Step the fixed scene kTicks times in the fixed system order, hashing the world after EVERY system
// (the per-system attribution axis) and folding each tick's end root into the trace (so a mid-run
// divergence that self-heals by the last tick still fails). `tamper`/`captures` drive the triage
// scenarios; both nullptr on golden runs.
RunTrace run_fixture(const Tamper* tamper = nullptr, std::vector<Capture>* captures = nullptr)
{
    Scene s;
    build_scene(s);

    RunTrace r;
    session::Fnv1a fold;
    for (int t = 0; t < kTicks; ++t)
    {
        // The fixed parameter schedule (this scene's injected input) — identical on every run.
        if (t == kParamTickWalkerIdle)
            CHECK(s.animation.set_param(s.world, s.walker, Fixed::from_int(2)) == nullptr);
        if (t == kParamTickHeroStride)
            CHECK(s.animation.set_param(s.world, s.hero, Fixed::from_int(6)) == nullptr);

        std::array<std::uint64_t, kSystemCount> per_system{};
        for (std::size_t sys = 0; sys < kSystemCount; ++sys)
        {
            switch (sys)
            {
            case 0: system_steer(s, t); break;
            case 1: CHECK(s.phys3.step(s.world, kDt) == nullptr); break;
            case 2: CHECK(s.phys2.step(s.world, kDt) == nullptr); break;
            case 3: CHECK(s.animation.step(s.world, kDt) == nullptr); break;
            default: CHECK(s.particles.step(s.world, kDt) == nullptr); break;
            }
            if (tamper != nullptr && tamper->tick == t && tamper->system == sys)
                tamper->fn(s);
            const session::StateHash h = session::hash_world(s.world, session::sim_components());
            per_system[sys] = h.root;
            if (captures != nullptr)
            {
                for (Capture& c : *captures)
                    if (c.tick == t && c.system == sys)
                        c.out = session::snapshot_world(s.world, session::sim_components());
            }
        }
        r.systems.push_back(per_system);
        r.roots.push_back(per_system.back());
        fold.update_u64(per_system.back());

        // Activity probe: a dynamic 3D sphere moving upward means a floor/ramp bounce happened.
        s.world.each<p3::Transform3d, p3::Velocity3d, p3::Body3d, p3::Collider3d>(
            [&](kernel::Entity, p3::Transform3d&, p3::Velocity3d& vel, p3::Body3d& body,
                p3::Collider3d&)
            {
                if (body.flags == p3::kBodyFlagDynamic && vel.vy > 0)
                    r.any_upward = true;
            });
    }
    r.final_root = session::hash_world(s.world, session::sim_components()).root;
    r.trace_fold = fold.digest();

    anim::RootMotionState rm;
    CHECK(anim::read_root_motion(s.world, s.hero, rm));
    r.hero_forward = rm.position.z.raw < 0 ? -rm.position.z.raw : rm.position.z.raw;

    s.world.each<parts::ParticleEmitter3d>([&](kernel::Entity, parts::ParticleEmitter3d& em)
                                           { r.total_emitted += em.emitted; });
    r.final_live = parts::particle_count(s.world);

    const p2::Transform2d* tr2 = s.world.get<p2::Transform2d>(s.steer2);
    CHECK(tr2 != nullptr);
    if (tr2 != nullptr)
    {
        const std::int64_t dx = tr2->px - Fixed::from_int(-3).raw;
        const std::int64_t dy = tr2->py - Fixed::from_int(4).raw;
        r.p2_travel = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }

    r.hero = s.hero;
    r.steer3 = s.steer3;
    r.steer2 = s.steer2;
    return r;
}

// The four-level (tick, system, entity, componentField) attribution the `context determinism diff`
// triage performs, exercised over the combined scene via the SAME primitives that command consumes
// (triage.h): replay-bisect the per-tick root ladders, walk the per-system hashes within the first
// divergent tick, then diff the canonical post-system snapshots field-by-field.
void check_attribution(const RunTrace& left, const RunTrace& right, const Tamper& tamper,
                       const session::WorldSnapshot& left_snap,
                       const session::WorldSnapshot& right_snap, kernel::Entity expected_entity,
                       const char* expected_component, const char* expected_field,
                       std::int64_t expected_delta)
{
    // 1. replay-bisect the two per-tick root-hash ladders to the FIRST divergent tick.
    const std::int64_t tick = session::bisect_first_divergent_tick(left.roots, right.roots);
    CHECK(tick == tamper.tick);

    // 2. within that tick, walk the per-system hashes to the first divergent system.
    std::size_t sys = kSystemCount;
    for (std::size_t i = 0; i < kSystemCount; ++i)
    {
        if (left.systems[static_cast<std::size_t>(tick)][i] !=
            right.systems[static_cast<std::size_t>(tick)][i])
        {
            sys = i;
            break;
        }
    }
    CHECK(sys == tamper.system);
    // sys == tamper.system (asserted above) is what pins the attributed system index to the
    // expected one; no separate name compare is needed.

    // 3. attribute to the concrete (entity, component, field) via the canonical snapshot diff — the
    //    R-LANG-010 schema names the package component + field.
    const session::FieldDivergence fd = session::first_field_divergence(left_snap, right_snap);
    CHECK(fd.found);
    CHECK(!fd.structural);
    CHECK(fd.entity_index == static_cast<std::uint64_t>(expected_entity.index));
    CHECK(fd.entity_generation == static_cast<std::uint64_t>(expected_entity.generation));
    CHECK(fd.component == expected_component);
    CHECK(fd.field == expected_field);
    CHECK(fd.right_value - fd.left_value == expected_delta);
}

// Run one injected-divergence exercise: tamper the RIGHT run at (tamper.tick, tamper.system),
// capturing the tampered post-system snapshot there, then attribute it against the clean LEFT run's
// snapshot at the same point — the four-level (tick, system, entity, componentField) triage.
void exercise_divergence(const RunTrace& left, const session::WorldSnapshot& left_snap,
                         const Tamper& tamper, kernel::Entity expected_entity,
                         const char* expected_component, const char* expected_field,
                         std::int64_t expected_delta)
{
    std::vector<Capture> right_caps = {Capture{tamper.tick, tamper.system, {}}};
    const RunTrace right = run_fixture(&tamper, &right_caps);
    check_attribution(left, right, tamper, left_snap, right_caps[0].out, expected_entity,
                      expected_component, expected_field, expected_delta);
}

// The controlled divergences: a physics drift (steer3's vertical velocity moved by a few raw
// sub-units right after the physics3d solve — an FMA-contraction-sized error) and an animation drift
// (the hero's accumulated root-motion pz moved right after the animation step).
constexpr std::int64_t kPhysDelta = 3;
constexpr std::int64_t kAnimDelta = 7;

void tamper_physics(Scene& s)
{
    p3::Velocity3d* v = s.world.get<p3::Velocity3d>(s.steer3);
    CHECK(v != nullptr);
    if (v != nullptr)
        v->vy += kPhysDelta;
}

void tamper_animation(Scene& s)
{
    anim::RootMotion* rm = s.world.get<anim::RootMotion>(s.hero);
    CHECK(rm != nullptr);
    if (rm != nullptr)
        rm->pz += kAnimDelta;
}

// The golden digests, derived on the reference build and asserted identical on every matrix platform
// (Linux-x64 / Win-x64 / macOS-ARM64).
constexpr std::uint64_t kGoldenFinalRoot = 0x120B11291D74B688ULL;
constexpr std::uint64_t kGoldenTraceFold = 0xFD438111604AD641ULL;
} // namespace

int main()
{
    const RunTrace a = run_fixture();

    // Print the observed digests so a deliberate golden update (or a platform mismatch in CI) is
    // legible directly from the test log.
    std::printf("[determinism-gameplay] ticks=%d emitted=%lld live=%zu heroForward=%lld "
                "finalRoot=0x%016llX traceFold=0x%016llX\n",
                kTicks, static_cast<long long>(a.total_emitted), a.final_live,
                static_cast<long long>(a.hero_forward),
                static_cast<unsigned long long>(a.final_root),
                static_cast<unsigned long long>(a.trace_fold));

    // --- within-run determinism: a second identical run reproduces every digest exactly ----------
    const RunTrace b = run_fixture();
    CHECK(b.final_root == a.final_root);
    CHECK(b.trace_fold == a.trace_fold);
    CHECK(session::bisect_first_divergent_tick(a.roots, b.roots) == -1); // triage: no divergence

    // --- the scene really is multi-system active (guards against a silently inert composition) ---
    CHECK(a.roots.size() == static_cast<std::size_t>(kTicks));
    CHECK(a.any_upward);                  // physics3d: some sphere bounced
    CHECK(a.p2_travel > 0);               // physics2d: the steered circle moved
    CHECK(a.hero_forward > 0);            // animation: the hero accumulated root motion
    CHECK(a.total_emitted > 0);           // particles: emitters produced particles
    CHECK(a.final_live > 0);              // ...some still alive at the end
    CHECK(a.final_live < static_cast<std::size_t>(a.total_emitted)); // ...and despawn happened

    // --- triage: a PHYSICS then an ANIMATION divergence, each attributed to (tick, system, entity,
    //     componentField). ONE clean LEFT run captures at BOTH injection points and is shared. ------
    {
        std::vector<Capture> left_caps = {Capture{57, 1, {}}, Capture{130, 3, {}}};
        const RunTrace left = run_fixture(nullptr, &left_caps);
        CHECK(left.final_root == a.final_root); // the capture run is the same clean run
        exercise_divergence(left, left_caps[0].out, Tamper{57, 1, &tamper_physics}, a.steer3,
                            "physics3d_velocity", "vy", kPhysDelta); // after the physics3d solve @57
        exercise_divergence(left, left_caps[1].out, Tamper{130, 3, &tamper_animation}, a.hero,
                            "anim_root_motion", "pz", kAnimDelta); // after the animation step @130
    }

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 -----
    CHECK(a.final_root == kGoldenFinalRoot);
    CHECK(a.trace_fold == kGoldenTraceFold);

    SESSION_TEST_MAIN_END();
}
