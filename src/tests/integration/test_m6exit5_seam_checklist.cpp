// M6 exit criterion 5 — the M6 CORE-SYSTEMS SEAM CHECKLIST as an executable audit (design
// §M6-EXIT, issue #197), registered as the blocking `m6-exit-5-seam-checklist` ctest and run by
// the CI build job's "M6 exit gate" named step on all three build-matrix legs. The milestone-
// closing mirror of the M2 data-model (m2-exit-6) and M5 observer-editor (m5-exit-3) seam
// checklists: ONE assertion per M6 seam, exercising the real public surface, so a regression that
// quietly drops a seam turns this milestone gate red. The seams (design § Seams; REQUIREMENTS
// R-SYS-001..007 / R-SIM-001/005/008 / R-NET-001; DESIGN L-45 / L-46 / L-48 / L-54 / L-60):
//
//   1  microkernel invariant — no src/kernel/ source includes a package header (L-60 layering:
//      packages compose ON the kernel; the kernel never reaches back)
//   2  every sim package composes on the ONE kernel World (physics3d + physics2d + animation +
//      particles + spline all active in a single world, no package-private entity store)
//   3  whole-build deterministic property — the attestation is PRODUCED from the actually-applied
//      strict-FP flags and fails CLOSED on fast-math / missing strict-FP / unrecorded flags
//      (R-SIM-005; the strict-FP flavor's deterministic:true is the blocking `deterministic` job)
//   4  package sim-components fold into the hierarchical state hash (registered by stable name in
//      sim_components(); a raw component mutation moves the L-54 root)
//   5  audio + cosmetic particles are presentation observers OFF the sim path (R-SIM-001: neither
//      moves the hierarchical hash)
//   6  input UI-vs-gameplay routing arbitration (R-SYS-007 / L-45: a capturing UI context swallows
//      gameplay input; popping it restores gameplay routing; routing is pure)
//   7  replication metadata bound to the composed id (L-48: the net identity IS the L-37 composed
//      id; duplicate/zero ids refuse fail-closed; deltas are keyed by it)
//   8  minter discipline — every M6 package's fail-closed codes live in its F0a-reserved catalog
//      domain block (pinned strings, so a rename desyncs from error_catalog.cpp and goes red)

#include "context/kernel/world.h"
#include "context/packages/animation/animation_world.h"
#include "context/packages/animation/errors.h"
#include "context/packages/audio/audio_engine.h"
#include "context/packages/audio/errors.h"
#include "context/packages/input/errors.h"
#include "context/packages/input/input_router.h"
#include "context/packages/particles/components.h"
#include "context/packages/particles/cosmetic.h"
#include "context/packages/particles/errors.h"
#include "context/packages/particles/particle_world.h"
#include "context/packages/physics2d/components.h"
#include "context/packages/physics2d/errors.h"
#include "context/packages/physics2d/physics_world.h"
#include "context/packages/physics3d/components.h"
#include "context/packages/physics3d/errors.h"
#include "context/packages/physics3d/physics_world.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/vec.h"
#include "context/packages/spline/errors.h"
#include "context/packages/spline/spline_world.h"
#include "context/runtime/determinism/attestation.h"
#include "context/runtime/netsync/errors.h"
#include "context/runtime/netsync/state_sync.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef CONTEXT_KERNEL_SRC_DIR
#error "CONTEXT_KERNEL_SRC_DIR (path to src/kernel/) must be defined by the build."
#endif

namespace kernel = context::kernel;
namespace session = context::runtime::session;
namespace determinism = context::runtime::determinism;
namespace net = context::runtime::netsync;
namespace sm = context::packages::simmath;
namespace p3 = context::packages::physics3d;
namespace p2 = context::packages::physics2d;
namespace anim = context::packages::animation;
namespace parts = context::packages::particles;
namespace spline = context::packages::spline;
namespace input = context::packages::input;
namespace audio = context::packages::audio;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
int g_failures = 0;
void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

const Fixed kDt = Fixed::from_ratio(1, 60);

// The minimal valid locomotion rig the combined determinism scene established (2 joints, an idle
// clip + a striding clip with sim-visible root motion, a 2-state parameter-gated graph).
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
    anim::Clip stride;
    stride.duration = kOne;
    stride.loop = true;
    stride.tracks = {{}, {}};
    stride.root_velocity = {kZero, kZero, Fixed::from_int(2)};
    stride.yaw_rate = Fixed::from_ratio(1, 8);
    rig.clips = {idle, stride};

    anim::GraphState st_idle;
    st_idle.clip = 0;
    st_idle.transitions = {anim::Transition{1, anim::CompareOp::greater_equal, Fixed::from_int(5),
                                            Fixed::from_ratio(1, 4)}};
    anim::GraphState st_stride;
    st_stride.clip = 1;
    st_stride.transitions = {
        anim::Transition{0, anim::CompareOp::less, Fixed::from_int(5), Fixed::from_ratio(1, 4)}};
    rig.graph.states = {st_idle, st_stride};
    rig.graph.initial = 0;
    return rig;
}
} // namespace

int main()
{
    // === Seam 1 — microkernel invariant: no src/kernel/ source includes a package header ==========
    {
        namespace fs = std::filesystem;
        const fs::path kernel_dir(CONTEXT_KERNEL_SRC_DIR);
        CHECK(fs::exists(kernel_dir));
        std::size_t scanned = 0;
        std::size_t offending_lines = 0;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(kernel_dir))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".h" && ext != ".hpp" && ext != ".cpp" && ext != ".cc")
                continue;
            ++scanned;
            std::ifstream in(entry.path(), std::ios::binary);
            CHECK(static_cast<bool>(in));
            std::string line;
            while (std::getline(in, line))
            {
                if (line.find("#include") != std::string::npos &&
                    line.find("context/packages/") != std::string::npos)
                {
                    std::fprintf(stderr, "kernel -> package include: %s: %s\n",
                                 entry.path().string().c_str(), line.c_str());
                    ++offending_lines;
                }
            }
        }
        CHECK(scanned > 0);           // fail-closed: an empty scan proves nothing
        CHECK(offending_lines == 0);  // the microkernel never reaches into a package
    }

    // === Seam 2 — every sim package composes on the ONE kernel World ==============================
    // (Built first; seams 4/5 assert over this same composed world.)
    kernel::World world;
    p3::PhysicsWorld3d phys3;
    p2::PhysicsWorld2d phys2;
    anim::AnimationWorld animation;
    parts::ParticleWorld particles;
    spline::SplineWorld splines;

    const kernel::Entity ball3 = world.create();
    const kernel::Entity circle2 = world.create();
    const kernel::Entity walker = world.create();
    const kernel::Entity fountain = world.create();
    const kernel::Entity rider = world.create();
    {
        // physics3d: a static floor + a dropped dynamic sphere.
        p3::BodyDesc floor;
        floor.position = {kZero, Fixed::from_int(-1), kZero};
        floor.is_static = true;
        floor.shape = p3::Shape::Box;
        floor.half_extents = {Fixed::from_int(10), kOne, Fixed::from_int(10)};
        CHECK(phys3.add_body(world, world.create(), floor) == nullptr);
        p3::BodyDesc sphere;
        sphere.position = {kZero, Fixed::from_int(4), kZero};
        CHECK(phys3.add_body(world, ball3, sphere) == nullptr);

        // physics2d: a static platform + a dropped dynamic circle.
        p2::BodyDesc platform;
        platform.position = {kZero, Fixed::from_int(-1)};
        platform.is_static = true;
        platform.shape = p2::Shape::Box;
        platform.half_extents = {Fixed::from_int(10), kOne};
        CHECK(phys2.add_body(world, world.create(), platform) == nullptr);
        p2::BodyDesc circle;
        circle.position = {kZero, Fixed::from_int(3)};
        CHECK(phys2.add_body(world, circle2, circle) == nullptr);

        // animation: a striding animator with sim-visible root motion.
        CHECK(animation.set_rig(make_rig()) == nullptr);
        CHECK(animation.add_animator(world, walker) == nullptr);
        CHECK(animation.set_param(world, walker, Fixed::from_int(6)) == nullptr);

        // particles: a deterministic fountain emitter.
        parts::EmitterDesc desc;
        desc.velocity = {kZero, Fixed::from_int(6), kZero};
        desc.spread = Fixed::from_ratio(1, 2);
        desc.rate = 3;
        desc.lifetime = 30;
        desc.seed = 0xC0FFEEu;
        CHECK(particles.add_emitter(world, fountain, desc) == nullptr);

        // spline: a catmull-rom path + a follower riding it.
        spline::Curve curve;
        curve.type = spline::CurveType::catmull_rom;
        curve.points = {sm::Vec3{kZero, kZero, kZero}, sm::Vec3{Fixed::from_int(2), kZero, kZero},
                        sm::Vec3{Fixed::from_int(4), kOne, kZero},
                        sm::Vec3{Fixed::from_int(6), kOne, kZero}};
        CHECK(splines.set_paths({curve}) == nullptr);
        CHECK(splines.add_follower(world, rider, 0, kOne, /*loop=*/true) == nullptr);

        // Step every package over the SAME world; each one's state stays readable (composed, not
        // forked into a package-private store).
        for (int t = 0; t < 30; ++t)
        {
            CHECK(phys3.step(world, kDt) == nullptr);
            CHECK(phys2.step(world, kDt) == nullptr);
            CHECK(animation.step(world, kDt) == nullptr);
            CHECK(particles.step(world, kDt) == nullptr);
            CHECK(splines.step(world, kDt) == nullptr);
        }
        p3::BodyState b3;
        CHECK(p3::read_body(world, ball3, b3));
        CHECK(b3.position.y.raw != Fixed::from_int(4).raw); // it genuinely fell
        p2::BodyState b2;
        CHECK(p2::read_body(world, circle2, b2));
        CHECK(b2.position.y.raw != Fixed::from_int(3).raw);
        anim::RootMotionState rm;
        CHECK(anim::read_root_motion(world, walker, rm));
        CHECK(rm.position.z.raw != 0); // the stride accumulated sim-visible root motion
        parts::EmitterState emitter;
        CHECK(parts::read_emitter(world, fountain, emitter));
        CHECK(emitter.emitted > 0);
        CHECK(parts::particle_count(world) > 0);
        spline::FollowerState follower;
        CHECK(spline::read_follower(world, rider, follower));
        CHECK(follower.distance.raw > 0); // the follower advanced along the path
    }

    // === Seam 3 — whole-build deterministic property: attestation from applied flags, fail-closed =
    {
        // THIS build's honest attestation: produced from the probe + the CMake-recorded flags. On
        // the default dev flavor nothing was requested -> an honest false with no failure code; on
        // the strict-FP `deterministic` flavor the produced result must be a verified true.
        const determinism::Attestation produced = determinism::produce_attestation();
        CHECK(produced.requested ? (produced.deterministic && produced.failure_code.empty())
                                 : (!produced.deterministic && produced.failure_code.empty()));
        CHECK(determinism::to_json(produced).find("\"deterministic\":") != std::string::npos);

        // The PURE decision fails closed on every violation class (R-SIM-005: the attestation can
        // never claim determinism the build did not deliver).
        determinism::FpProbe fast;
        fast.deterministic_requested = true;
        fast.fast_math = true;
        CHECK(determinism::verify_attestation(fast, "-ffp-contract=off").failure_code ==
              determinism::kAttestationFastMathForbidden);

        determinism::FpProbe lax_msvc;
        lax_msvc.deterministic_requested = true;
        lax_msvc.is_msvc = true;
        lax_msvc.msvc_strict_fp = false;
        CHECK(determinism::verify_attestation(lax_msvc, "/fp:strict").failure_code ==
              determinism::kAttestationStrictFpMissing);

        determinism::FpProbe unrecorded;
        unrecorded.deterministic_requested = true;
        CHECK(determinism::verify_attestation(unrecorded, "").failure_code ==
              determinism::kAttestationFlagsUnverified);

        determinism::FpProbe clean;
        clean.deterministic_requested = true;
        const determinism::Attestation ok =
            determinism::verify_attestation(clean, "-ffp-contract=off");
        CHECK(ok.deterministic && ok.failure_code.empty());
    }

    // === Seam 4 — package sim-components fold into the hierarchical state hash ====================
    {
        // Every M6 package registered its sim components by STABLE NAME into the combined registry
        // (the seam the hierarchical hash + triage attribute through, R-LANG-010).
        const session::SimComponentRegistry& reg = session::sim_components();
        for (const char* name :
             {p3::kTransformComponentName, p3::kVelocityComponentName, p3::kBodyComponentName,
              p3::kColliderComponentName, p2::kTransformComponentName, p2::kVelocityComponentName,
              p2::kBodyComponentName, p2::kColliderComponentName, anim::kAnimatorComponentName,
              anim::kRootMotionComponentName, parts::kParticleComponentName,
              parts::kEmitterComponentName, spline::kFollowerComponentName})
            CHECK(reg.by_name(name) != nullptr);

        // ...and a raw package-component mutation MOVES the L-54 root (then restores exactly).
        const std::uint64_t before = session::hash_world(world, reg).root;
        p3::Velocity3d* vel = world.get<p3::Velocity3d>(ball3);
        CHECK(vel != nullptr);
        if (vel != nullptr)
        {
            vel->vy += 1;
            CHECK(session::hash_world(world, reg).root != before);
            vel->vy -= 1;
            CHECK(session::hash_world(world, reg).root == before);
        }
    }

    // === Seam 5 — audio + cosmetic particles are presentation observers OFF the sim path ==========
    {
        const std::uint64_t before = session::hash_world(world, session::sim_components()).root;

        // Audio (R-SYS-006 / L-46): a whole bus-graph + spatialized event + null-backend mix — the
        // sim world's hash never moves (audio reads sim state; it never writes it).
        audio::AudioEngine engine;
        CHECK(engine.init() == nullptr);
        CHECK(engine.configure_buses({audio::BusConfig{"master", 1.0F, -1},
                                      audio::BusConfig{"sfx", 0.9F, 0}}) == nullptr);
        engine.set_listener(audio::Vec3{0.0F, 2.0F, 0.0F});
        audio::EventDesc ping;
        ping.bus = 1;
        ping.gain = 1.0F;
        ping.spatial = true;
        ping.min_distance = 1.0F;
        ping.max_distance = 20.0F;
        ping.life_seconds = 0.25F;
        CHECK(engine.trigger(ping, audio::Vec3{1.0F, 0.0F, 0.0F}) == nullptr);
        std::vector<float> mix;
        engine.render_for_test(mix, 128, 48000);
        CHECK(engine.active_voice_count() >= 1);

        // Cosmetic particles (R-SYS-003 / R-SIM-001): observe the live sim particles, free-run the
        // float presentation state — no World write, no hash movement.
        parts::CosmeticParticleSystem cosmetic;
        cosmetic.observe(world);
        CHECK(cosmetic.live_count() > 0);
        cosmetic.advance(1.0F / 60.0F);

        CHECK(session::hash_world(world, session::sim_components()).root == before);
    }

    // === Seam 6 — input UI-vs-gameplay routing arbitration (R-SYS-007 / L-45) ======================
    {
        input::InputRouter router;
        CHECK(router.install_context(input::InputContext{
                  "gameplay",
                  input::Layer::Gameplay,
                  false,
                  {{"keyboard", "D", "move_x"}}}) == nullptr);
        CHECK(router.install_context(input::InputContext{
                  "pause",
                  input::Layer::Ui,
                  /*capture=*/true,
                  {{"keyboard", "Escape", "ui_menu"}}}) == nullptr);
        CHECK(router.push_context("gameplay") == nullptr);

        const std::vector<session::InputEvent> press_d = {{"keyboard", "D", 1}};
        const std::vector<session::InputEvent> press_esc = {{"keyboard", "Escape", 1}};

        // Gameplay owns its binding while no UI captures.
        session::TickInputs routed = router.route(1, press_d);
        CHECK(routed.actions.size() == 1 && routed.actions[0].action == "move_x");

        // A capturing UI context on top SWALLOWS unbound gameplay input (modal capture)...
        CHECK(router.push_context("pause") == nullptr);
        routed = router.route(2, press_d);
        CHECK(routed.actions.empty());
        // ...while its own binding routes as the UI action.
        routed = router.route(3, press_esc);
        CHECK(routed.actions.size() == 1 && routed.actions[0].action == "ui_menu");

        // Popping the UI context restores gameplay routing; routing is pure (same in, same out).
        CHECK(router.pop_context() == nullptr);
        routed = router.route(4, press_d);
        CHECK(routed.actions.size() == 1 && routed.actions[0].action == "move_x");
        const session::TickInputs again = router.route(4, press_d);
        CHECK(again.actions.size() == routed.actions.size());
    }

    // === Seam 7 — replication metadata bound to the composed id (L-48 / L-37) ======================
    {
        kernel::World net_world;
        const kernel::Entity body = net_world.create();
        net_world.add<p3::Velocity3d>(body, p3::Velocity3d{});

        constexpr std::uint64_t kComposedId = 0x0123456789ABCDEFULL; // a composed-id-shaped identity
        net::NetIdMap registry;
        CHECK(net::register_replicated(net_world, body, kComposedId, /*authority=*/1, registry) ==
              nullptr);
        CHECK(net_world.replication_of(body) != nullptr);
        CHECK(net_world.replication_of(body)->net_id == kComposedId); // bound to the composed id
        CHECK(net_world.replication_of(body)->authority == 1);

        // Fail-closed identity discipline: a duplicate composed id and a zero id both refuse.
        const kernel::Entity second = net_world.create();
        CHECK(net::register_replicated(net_world, second, kComposedId, 0, registry) != nullptr);
        CHECK(net::register_replicated(net_world, second, 0, 0, registry) != nullptr);

        // The dirty/delta capture is keyed by that composed id.
        net::ReplicatedComponentSet set;
        set.add<p3::Velocity3d>();
        const net::StateSyncSnapshot snap = net::capture_delta(net_world, set, 0);
        CHECK(snap.entities.size() == 1);
        CHECK(snap.entities[0].net_id == kComposedId);
    }

    // === Seam 8 — minter discipline: every package fills ONLY its F0a-reserved catalog block =======
    {
        // The F0a pre-reserved M6 domain blocks (error_catalog.cpp): each package is the single
        // minter for its own domain. The strings are pinned so a rename/mis-domain desyncs loudly.
        CHECK(std::string(p3::kInvalidEntityCode) == "physics3d.invalid_entity");
        CHECK(std::string(p3::kInvalidStepCode) == "physics3d.invalid_step");
        CHECK(std::string(p2::kInvalidEntityCode) == "physics2d.invalid_entity");
        CHECK(std::string(p2::kInvalidStepCode) == "physics2d.invalid_step");
        CHECK(std::string(anim::kInvalidRigCode) == "anim.invalid_rig");
        CHECK(std::string(anim::kInvalidStepCode) == "anim.invalid_step");
        CHECK(std::string(parts::kInvalidConfigCode) == "particle.invalid_config");
        CHECK(std::string(parts::kInvalidStepCode) == "particle.invalid_step");
        CHECK(std::string(spline::kInvalidPathCode) == "spline.invalid_path");
        CHECK(std::string(spline::kInvalidStepCode) == "spline.invalid_step");
        CHECK(std::string(audio::kInvalidBusCode) == "audio.invalid_bus");
        CHECK(std::string(audio::kDeviceUnavailableCode) == "audio.device_unavailable");
        CHECK(std::string(input::kInvalidContextCode) == "input.invalid_context");
        CHECK(std::string(input::kUnknownActionCode) == "input.unknown_action");
        CHECK(std::string(net::kInvalidNetIdCode) == "net.invalid_net_id");
        CHECK(std::string(net::kAuthorityConflictCode) == "net.authority_conflict");
        CHECK(std::string(determinism::kAttestationFastMathForbidden) ==
              "determinism.attestation_fastmath_forbidden");
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("m6-exit-5-seam-checklist: all M6 seams held\n");
    return 0;
}
