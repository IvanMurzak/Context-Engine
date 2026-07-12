// The roll-3d SAMPLE-GAME SMOKE GATE (M6 EXIT-A, issue #194; R-QA-005 / R-QA-013 / R-HEAD-002) —
// the per-game headless proof the EXIT-B m6-exit-1 gate rides: the authored samples/roll-3d project
// builds through the REAL RuntimeKernel session surface (the "roll-3d" registered scenario parses
// the authored arena layout) and headless-steps N fixed ticks with NO GPU and NO audio device —
// while every landed gameplay package visibly does its job:
//   * P7 input   — raw keyboard events routed through the REAL context_input router (contexts
//                  mirroring roll.input-bindings.json) into the session's injection sink; the
//                  mapped move actions drive the ball;
//   * P1 physics — the ball + boulders fall, bounce and roll on the floor/ramp (context_spatial
//                  broad-phase inside the package);
//   * P4 particles — each ball floor-impact opens a deterministic particle burst;
//   * P3 animation — the flag prop's anim-graph transitions idle -> wave once the impact count
//                  crosses the authored threshold, accumulating sim-visible root motion;
//   * P6 audio   — each impact triggers the authored impact event on miniaudio's NULL backend
//                  (presentation observer, OFF the sim path — R-SIM-001).
// Progress is asserted against the R-QA-005 simTick counter + state queries, then within-run
// determinism (a second identical run reproduces every digest) and the CROSS-PLATFORM golden
// (integer/fixed-point sim + fixed-endian fold => portable digests, the L-54 wedge law).
//
// Updating the goldens: they change only when the scene, a package stepper, or the input schedule
// changes ON PURPOSE. Re-derive by running this gate — it prints the observed values — then paste.

#include "sample_games.h"

#include "context/packages/animation/animation_world.h"
#include "context/packages/audio/audio_engine.h"
#include "context/packages/input/input_router.h"
#include "context/packages/particles/particle_world.h"
#include "context/packages/physics3d/physics_world.h"
#include "context/runtime/session/hash.h"
#include "context/runtime/session/session.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef CONTEXT_SAMPLES_DIR
#error "CONTEXT_SAMPLES_DIR (path to the samples/ corpus root) must be defined by the build."
#endif

namespace games = context::tests::games;
namespace session = context::runtime::session;
namespace kernel = context::kernel;
namespace input = context::packages::input;
namespace audio = context::packages::audio;
namespace p3 = context::packages::physics3d;
namespace anim = context::packages::animation;
namespace parts = context::packages::particles;

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

constexpr int kTicks = 240;

// The fixed input schedule (the recorded "player"): let the spawn drop's bounces settle on the
// springy floor first (the impact/burst/sound/flag beats), then roll the ball across the open
// floor — D (x+) away from the ramp, then W (z+) — and release to coast. Values are the axis
// magnitudes the authored bindings map onto move_x / move_y; a gentle 1 keeps the ball well inside
// the 40x40 floor for the whole run.
std::vector<session::InputEvent> raw_for_tick(int t)
{
    std::vector<session::InputEvent> raw;
    if (t == 100)
        raw.push_back(session::InputEvent{"keyboard", "D", 1});
    else if (t == 140)
        raw.push_back(session::InputEvent{"keyboard", "D", 0}); // release (held state persists)
    else if (t == 150)
        raw.push_back(session::InputEvent{"keyboard", "W", 1});
    else if (t == 190)
        raw.push_back(session::InputEvent{"keyboard", "W", 0}); // release; coast to the end
    return raw;
}

// Install the router contexts mirroring samples/roll-3d/input/roll.input-bindings.json (the
// authored L-33 ids are the mapped action names — see that file's notes for the human labels).
void install_contexts(input::InputRouter& router)
{
    CHECK(router.install_context(input::InputContext{
              games::kRollContextGameplay,
              input::Layer::Gameplay,
              false,
              {{"keyboard", "A", games::kRollActionMoveX},
               {"keyboard", "D", games::kRollActionMoveX},
               {"keyboard", "W", games::kRollActionMoveY},
               {"keyboard", "S", games::kRollActionMoveY}}}) == nullptr);
    CHECK(router.install_context(input::InputContext{
              games::kRollContextPause,
              input::Layer::Ui,
              /*capture=*/true,
              {{"keyboard", "Escape", games::kRollActionUiMenu}}}) == nullptr);
    CHECK(router.push_context(games::kRollContextGameplay) == nullptr);
}

struct RunResult
{
    std::uint64_t final_sim_tick = 0;
    std::uint64_t final_root = 0;
    std::uint64_t trace_fold = 0;

    // Activity read-back (proves every package genuinely did its job).
    std::int64_t impacts = 0;
    std::int64_t ball_travel = 0;   // |dx| + |dz| of the ball from its authored spawn (raw Q16)
    std::int64_t boulder_a_travel = 0;
    std::int64_t boulder_b_travel = 0;
    std::int64_t emitted = 0;       // burst emitter's running emission count
    std::size_t max_live = 0;       // peak live particle count over the run
    int flag_state = -1;            // the prop's anim-graph state at the end
    std::int64_t flag_yaw_raw = 0;  // |accumulated root-motion yaw| of the prop (raw Q16)
    std::int64_t sounds_fired = 0;  // impact sounds triggered on the null backend
    std::size_t max_voices = 0;     // peak live audio voices observed
    bool mixed_nonzero = false;     // the null-backend mixer produced a non-silent sample
};

RunResult run_fixture()
{
    RunResult r;

    // The authored layout is ALSO the presentation anchor: the audio listener sits at the authored
    // camera position (the one-way int->float conversion is the R-SIM-001 presentation boundary).
    std::vector<games::NamedPosition> layout;
    CHECK(games::load_scene_layout(std::string(CONTEXT_SAMPLES_DIR) +
                                       "/roll-3d/scenes/arena.scene.json",
                                   layout));
    const games::NamedPosition* camera = games::find_entity(layout, "MainCamera");
    CHECK(camera != nullptr);

    // The REAL session surface: the registered "roll-3d" scenario builds the game world.
    session::Session s(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "roll-3d"});
    CHECK(s.system_names().size() == 6); // input, control, physics3d, impact, particles, animation

    const games::RollGame game0 = games::read_roll_game(s.world());
    CHECK(game0.ball_gen != 0); // the scenario factory built the world (fail-closed otherwise)
    const kernel::Entity ball = games::entity_from(game0.ball_index, game0.ball_gen);
    const kernel::Entity prop = games::entity_from(game0.prop_index, game0.prop_gen);
    const kernel::Entity burst = games::entity_from(game0.burst_index, game0.burst_gen);
    const kernel::Entity boulder_a = games::entity_from(game0.boulder_a_index, game0.boulder_a_gen);
    const kernel::Entity boulder_b = games::entity_from(game0.boulder_b_index, game0.boulder_b_gen);

    p3::BodyState ball_spawn;
    p3::BodyState boulder_a_spawn;
    p3::BodyState boulder_b_spawn;
    CHECK(p3::read_body(s.world(), ball, ball_spawn));
    CHECK(p3::read_body(s.world(), boulder_a, boulder_a_spawn));
    CHECK(p3::read_body(s.world(), boulder_b, boulder_b_spawn));

    // P7: the REAL routing front-end, contexts mirroring the authored bindings.
    input::InputRouter router;
    install_contexts(router);

    // P6: the null-backend audio engine, bus graph mirroring game-mix.audio-bus.json and the
    // impact EventDesc mirroring impact.audio-event.json. Entirely OFF the sim path.
    audio::AudioEngine engine;
    CHECK(engine.init() == nullptr);
    CHECK(engine.configure_buses({audio::BusConfig{"bbb1c5526b8a8280", 1.0F, -1},
                                  audio::BusConfig{"ac936623e44e8bfb", 0.9F, 0}}) == nullptr);
    engine.set_listener(audio::Vec3{static_cast<float>(camera->x),
                                    static_cast<float>(camera->y),
                                    static_cast<float>(camera->z)});
    audio::EventDesc impact_event;
    impact_event.bus = 1; // Sound Effects
    impact_event.gain = 1.0F;
    impact_event.spatial = true;
    impact_event.min_distance = 1.0F;
    impact_event.max_distance = 30.0F;
    impact_event.life_seconds = 0.25F;

    // The presentation hook: in the inter-tick gap (the R-HEAD-002 service point), fire the impact
    // sound for every newly recorded ball impact, spatialized at the ball's position. Reads sim
    // state, never writes it.
    std::int64_t seen_impacts = 0;
    std::vector<float> mix;
    s.set_inter_tick_hook(
        [&](std::uint64_t)
        {
            const games::RollGame g = games::read_roll_game(s.world());
            if (g.impacts <= seen_impacts)
                return;
            const p3::Transform3d* tr = s.world().get<p3::Transform3d>(ball);
            CHECK(tr != nullptr);
            if (tr == nullptr)
                return;
            const float to_units = 1.0F / 65536.0F;
            const audio::Vec3 at{static_cast<float>(tr->px) * to_units,
                                 static_cast<float>(tr->py) * to_units,
                                 static_cast<float>(tr->pz) * to_units};
            for (std::int64_t i = seen_impacts; i < g.impacts; ++i)
            {
                CHECK(engine.trigger(impact_event, at) == nullptr);
                ++r.sounds_fired;
            }
            seen_impacts = g.impacts;
            r.max_voices = std::max(r.max_voices, engine.active_voice_count());
            engine.render_for_test(mix, 128, 48000);
            for (const float sample : mix)
                if (sample != 0.0F)
                    r.mixed_nonzero = true;
        });

    // --- the run: route raw device events, inject, step — one fixed tick at a time ---------------
    session::Fnv1a fold;
    for (int t = 0; t < kTicks; ++t)
    {
        const session::TickInputs routed = router.route(s.sim_tick(), raw_for_tick(t));
        for (const session::ActionActivation& a : routed.actions)
            s.inject_action_at(routed.tick, a);

        const session::StepResult sr = s.step(1);
        fold.update_u64(sr.state_hash.root);
        r.final_sim_tick = sr.sim_tick;
        r.max_live = std::max(r.max_live, parts::particle_count(s.world()));
    }
    r.final_root = s.state_hash().root;
    r.trace_fold = fold.digest();

    // --- read back the activity ------------------------------------------------------------------
    const games::RollGame game = games::read_roll_game(s.world());
    r.impacts = game.impacts;

    p3::BodyState ball_end;
    p3::BodyState boulder_a_end;
    p3::BodyState boulder_b_end;
    CHECK(p3::read_body(s.world(), ball, ball_end));
    CHECK(p3::read_body(s.world(), boulder_a, boulder_a_end));
    CHECK(p3::read_body(s.world(), boulder_b, boulder_b_end));
    const auto travel = [](const p3::BodyState& from, const p3::BodyState& to)
    {
        const std::int64_t dx = to.position.x.raw - from.position.x.raw;
        const std::int64_t dz = to.position.z.raw - from.position.z.raw;
        return (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    };
    r.ball_travel = travel(ball_spawn, ball_end);
    r.boulder_a_travel = travel(boulder_a_spawn, boulder_a_end);
    r.boulder_b_travel = travel(boulder_b_spawn, boulder_b_end);

    parts::EmitterState emitter;
    CHECK(parts::read_emitter(s.world(), burst, emitter));
    r.emitted = emitter.emitted;

    anim::AnimatorState animator;
    CHECK(anim::read_animator(s.world(), prop, animator));
    r.flag_state = animator.state;
    anim::RootMotionState rm;
    CHECK(anim::read_root_motion(s.world(), prop, rm));
    r.flag_yaw_raw = rm.yaw.raw < 0 ? -rm.yaw.raw : rm.yaw.raw;

    return r;
}

// The golden digests, derived on the reference build and asserted identical on every matrix
// platform (Linux-x64 / Win-x64 / macOS-ARM64).
constexpr std::uint64_t kGoldenFinalRoot = 0x74E891522157C3BFULL;
constexpr std::uint64_t kGoldenTraceFold = 0x50B6ED595239FF13ULL;
} // namespace

int main()
{
    games::register_roll3d_scenario(CONTEXT_SAMPLES_DIR);

    const RunResult a = run_fixture();

    std::printf("[game-smoke-roll-3d] ticks=%llu impacts=%lld emitted=%lld maxLive=%zu "
                "flagState=%d sounds=%lld finalRoot=0x%016llX traceFold=0x%016llX\n",
                static_cast<unsigned long long>(a.final_sim_tick),
                static_cast<long long>(a.impacts), static_cast<long long>(a.emitted), a.max_live,
                a.flag_state, static_cast<long long>(a.sounds_fired),
                static_cast<unsigned long long>(a.final_root),
                static_cast<unsigned long long>(a.trace_fold));

    // --- the session surface: exactly N fixed ticks ran (R-QA-005 simTick) -----------------------
    CHECK(a.final_sim_tick == static_cast<std::uint64_t>(kTicks));

    // --- P7: the routed input actually drove the player ------------------------------------------
    CHECK(a.ball_travel > (2LL << 16)); // the ball rolled well over 2 world units

    // --- P1: physics is genuinely active (the player bounced; free bodies moved) -----------------
    CHECK(a.impacts >= games::kRollFlagWaveThreshold);
    CHECK(a.boulder_a_travel > 0);
    CHECK(a.boulder_b_travel > 0);

    // --- P4: the impact bursts emitted (and some particles were alive mid-run) -------------------
    CHECK(a.emitted > 0);
    CHECK(a.max_live > 0);

    // --- P3: the flag's anim-graph crossed the authored threshold and accumulated root motion ----
    CHECK(a.flag_state == games::kRollFlagStateWave);
    CHECK(a.flag_yaw_raw > 0);

    // --- P6: impact sounds fired on the NULL backend (presentation observer, off the sim path) ---
    CHECK(a.sounds_fired == a.impacts);
    CHECK(a.max_voices >= 1);
    CHECK(a.mixed_nonzero);

    // --- within-run determinism: a second identical run reproduces every digest exactly ----------
    const RunResult b = run_fixture();
    CHECK(b.final_root == a.final_root);
    CHECK(b.trace_fold == a.trace_fold);
    CHECK(b.impacts == a.impacts);

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 -----
    CHECK(a.final_root == kGoldenFinalRoot);
    CHECK(a.trace_fold == kGoldenTraceFold);

    return g_failures == 0 ? 0 : 1;
}
