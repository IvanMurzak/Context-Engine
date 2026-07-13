// M6 exit criterion 1 — `m6-exit-1-games-run` (design §M6-EXIT, issue #197; R-QA-005 / R-HEAD-002 /
// R-SIM-001): BOTH EXIT-A sample games (samples/roll-3d/, samples/platformer-2d/) build and
// headless-step N fixed ticks through the shipped RuntimeKernel session surface — with moving/
// animated physics objects, particles emitting, audio events firing on miniaudio's NULL backend,
// and injected input driving the player — asserted against the R-QA-005 simTick counter and state
// queries. No GPU, no audio device, sim path integer/fixed-point end to end.
//
// RIDES the landed EXIT-A machinery (issue #194): the games/ scenario mirrors register each game as
// a first-class session SCENARIO that parses its authored samples/<game>/ scene for the entity
// layout, and the REAL context_input router + null-backend audio engine front the run exactly as
// the per-game smoke gates do. The CROSS-PLATFORM golden digests stay asserted by those per-game
// smokes (game-smoke-roll-3d / game-smoke-platformer-2d — one maintenance home for the goldens);
// THIS gate is the formal milestone-closing exit assertion that both games RUN with every landed
// gameplay package visibly active, blocking on all three build-matrix legs as the "M6 exit gate"
// CI step.

#include "sample_games.h"

#include "context/packages/animation/animation_world.h"
#include "context/packages/audio/audio_engine.h"
#include "context/packages/input/input_router.h"
#include "context/packages/particles/particle_world.h"
#include "context/packages/physics2d/physics_world.h"
#include "context/packages/physics3d/physics_world.h"
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
namespace p2 = context::packages::physics2d;
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

// Shared per-run activity summary (what "the game genuinely ran with every package active" means).
struct Activity
{
    std::uint64_t final_sim_tick = 0;
    std::int64_t player_travel = 0; // |displacement| of the input-driven player body (raw Q16)
    std::int64_t emitted = 0;       // the game's particle emitter's running emission count
    std::size_t max_live = 0;       // peak live particle count over the run
    std::int64_t sounds_fired = 0;  // audio events triggered on the null backend
    std::size_t max_voices = 0;
    bool mixed_nonzero = false;     // the null-backend mixer produced a non-silent sample
};

// --- roll-3d ---------------------------------------------------------------------------------------

// The fixed input schedule the roll-3d smoke gate established: settle the spawn drop, roll D (x+)
// then W (z+), release and coast.
std::vector<session::InputEvent> roll_raw_for_tick(int t)
{
    std::vector<session::InputEvent> raw;
    if (t == 100)
        raw.push_back(session::InputEvent{"keyboard", "D", 1});
    else if (t == 140)
        raw.push_back(session::InputEvent{"keyboard", "D", 0});
    else if (t == 150)
        raw.push_back(session::InputEvent{"keyboard", "W", 1});
    else if (t == 190)
        raw.push_back(session::InputEvent{"keyboard", "W", 0});
    return raw;
}

void run_roll3d()
{
    Activity r;

    std::vector<games::NamedPosition> layout;
    CHECK(games::load_scene_layout(std::string(CONTEXT_SAMPLES_DIR) +
                                       "/roll-3d/scenes/arena.scene.json",
                                   layout));
    const games::NamedPosition* camera = games::find_entity(layout, "MainCamera");
    CHECK(camera != nullptr);

    // The shipped RuntimeKernel session surface: the registered "roll-3d" scenario builds the world.
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

    // P7: the REAL routing front-end, contexts mirroring roll.input-bindings.json.
    input::InputRouter router;
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

    // P6: the null-backend audio engine (no audio device), bus graph + impact event mirroring the
    // authored samples/roll-3d/audio/ kind files. Entirely OFF the sim path (R-SIM-001).
    audio::AudioEngine engine;
    CHECK(engine.init() == nullptr);
    CHECK(engine.configure_buses({audio::BusConfig{"bbb1c5526b8a8280", 1.0F, -1},
                                  audio::BusConfig{"ac936623e44e8bfb", 0.9F, 0}}) == nullptr);
    engine.set_listener(audio::Vec3{static_cast<float>(camera->x), static_cast<float>(camera->y),
                                    static_cast<float>(camera->z)});
    audio::EventDesc impact_event;
    impact_event.bus = 1;
    impact_event.gain = 1.0F;
    impact_event.spatial = true;
    impact_event.min_distance = 1.0F;
    impact_event.max_distance = 30.0F;
    impact_event.life_seconds = 0.25F;

    // The presentation hook in the inter-tick gap (the R-HEAD-002 service point): fire the impact
    // sound for each newly recorded ball impact. Reads sim state, never writes it.
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

    // The run: route raw device events, inject, headless-step — one fixed tick at a time.
    for (int t = 0; t < kTicks; ++t)
    {
        const session::TickInputs routed = router.route(s.sim_tick(), roll_raw_for_tick(t));
        for (const session::ActionActivation& a : routed.actions)
            s.inject_action_at(routed.tick, a);
        const session::StepResult sr = s.step(1);
        r.final_sim_tick = sr.sim_tick;
        r.max_live = std::max(r.max_live, parts::particle_count(s.world()));
    }

    // --- the R-QA-005 session surface: exactly N fixed ticks ran ---------------------------------
    CHECK(r.final_sim_tick == static_cast<std::uint64_t>(kTicks));

    // --- state queries: every landed package visibly did its job ---------------------------------
    const games::RollGame game = games::read_roll_game(s.world());

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
    r.player_travel = travel(ball_spawn, ball_end);
    CHECK(r.player_travel > (2LL << 16)); // P7: the injected input drove the ball over 2 units

    CHECK(game.impacts >= games::kRollFlagWaveThreshold); // P1: the ball genuinely bounced
    CHECK(travel(boulder_a_spawn, boulder_a_end) > 0);    // P1: free bodies moved
    CHECK(travel(boulder_b_spawn, boulder_b_end) > 0);

    parts::EmitterState emitter;
    CHECK(parts::read_emitter(s.world(), burst, emitter));
    r.emitted = emitter.emitted;
    CHECK(r.emitted > 0);  // P4: the impact bursts emitted
    CHECK(r.max_live > 0); // ...and particles were alive mid-run

    anim::AnimatorState animator;
    CHECK(anim::read_animator(s.world(), prop, animator));
    CHECK(animator.state == games::kRollFlagStateWave); // P3: the anim-graph crossed the threshold
    anim::RootMotionState rm;
    CHECK(anim::read_root_motion(s.world(), prop, rm));
    CHECK(rm.yaw.raw != 0); // P3: sim-visible root motion accumulated

    CHECK(r.sounds_fired == game.impacts); // P6: every impact fired its event on the null backend
    CHECK(r.max_voices >= 1);
    CHECK(r.mixed_nonzero);

    std::printf("[m6-exit-1] roll-3d: ticks=%llu impacts=%lld emitted=%lld sounds=%lld\n",
                static_cast<unsigned long long>(r.final_sim_tick),
                static_cast<long long>(game.impacts), static_cast<long long>(r.emitted),
                static_cast<long long>(r.sounds_fired));
}

// --- platformer-2d ---------------------------------------------------------------------------------

// The fixed input schedule the platformer-2d smoke gate established: run right, tap jump twice,
// release and coast (the rightward run shoves the crate and crosses the coin).
std::vector<session::InputEvent> plat_raw_for_tick(int t)
{
    std::vector<session::InputEvent> raw;
    if (t == 10)
        raw.push_back(session::InputEvent{"keyboard", "D", 1});
    else if (t == 60)
        raw.push_back(session::InputEvent{"keyboard", "Space", 1});
    else if (t == 61)
        raw.push_back(session::InputEvent{"keyboard", "Space", 0});
    else if (t == 140)
        raw.push_back(session::InputEvent{"keyboard", "Space", 1});
    else if (t == 150)
        raw.push_back(session::InputEvent{"keyboard", "Space", 0});
    else if (t == 200)
        raw.push_back(session::InputEvent{"keyboard", "D", 0});
    return raw;
}

void run_platformer2d()
{
    Activity r;

    std::vector<games::NamedPosition> layout;
    CHECK(games::load_scene_layout(std::string(CONTEXT_SAMPLES_DIR) +
                                       "/platformer-2d/scenes/level-1.scene.json",
                                   layout));
    const games::NamedPosition* camera = games::find_entity(layout, "MainCamera");
    CHECK(camera != nullptr);

    session::Session s(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "platformer-2d"});
    CHECK(s.system_names().size() == 6); // input, control, physics2d, gameplay, particles, animation

    const games::PlatformerGame game0 = games::read_platformer_game(s.world());
    CHECK(game0.player_gen != 0);
    const kernel::Entity player = games::entity_from(game0.player_index, game0.player_gen);
    const kernel::Entity crate = games::entity_from(game0.crate_index, game0.crate_gen);
    const kernel::Entity barrel = games::entity_from(game0.barrel_index, game0.barrel_gen);
    const kernel::Entity puff = games::entity_from(game0.puff_index, game0.puff_gen);

    p2::BodyState player_spawn;
    p2::BodyState crate_spawn;
    p2::BodyState barrel_spawn;
    CHECK(p2::read_body(s.world(), player, player_spawn));
    CHECK(p2::read_body(s.world(), crate, crate_spawn));
    CHECK(p2::read_body(s.world(), barrel, barrel_spawn));

    // P7: the REAL routing front-end, contexts mirroring player.input-bindings.json.
    input::InputRouter router;
    CHECK(router.install_context(input::InputContext{
              games::kPlatContextGameplay,
              input::Layer::Gameplay,
              false,
              {{"keyboard", "A", games::kPlatActionMoveX},
               {"keyboard", "D", games::kPlatActionMoveX},
               {"keyboard", "Space", games::kPlatActionJump},
               {"gamepad", "ButtonSouth", games::kPlatActionJump}}}) == nullptr);
    CHECK(router.install_context(input::InputContext{
              games::kPlatContextPause,
              input::Layer::Ui,
              /*capture=*/true,
              {{"keyboard", "Escape", games::kPlatActionUiMenu}}}) == nullptr);
    CHECK(router.push_context(games::kPlatContextGameplay) == nullptr);

    // P6: the null-backend audio engine, buses + jump/coin events mirroring the authored kinds.
    audio::AudioEngine engine;
    CHECK(engine.init() == nullptr);
    CHECK(engine.configure_buses({audio::BusConfig{"66f78214eb2942ad", 1.0F, -1},
                                  audio::BusConfig{"5a08393cca219c68", 0.9F, 0}}) == nullptr);
    engine.set_listener(audio::Vec3{static_cast<float>(camera->x), static_cast<float>(camera->y),
                                    static_cast<float>(camera->z)});
    audio::EventDesc jump_event;
    jump_event.bus = 1;
    jump_event.gain = 1.0F;
    jump_event.spatial = true;
    jump_event.min_distance = 1.0F;
    jump_event.max_distance = 20.0F;
    jump_event.life_seconds = 0.25F;
    audio::EventDesc coin_event;
    coin_event.bus = 1;
    coin_event.gain = 0.8F;
    coin_event.spatial = false;
    coin_event.life_seconds = 0.25F;

    std::int64_t seen_jumps = 0;
    std::int64_t seen_coin = 0;
    std::vector<float> mix;
    s.set_inter_tick_hook(
        [&](std::uint64_t)
        {
            const games::PlatformerGame g = games::read_platformer_game(s.world());
            bool triggered = false;
            if (g.jumps > seen_jumps)
            {
                const p2::Transform2d* tr = s.world().get<p2::Transform2d>(player);
                CHECK(tr != nullptr);
                const float to_units = 1.0F / 65536.0F;
                const audio::Vec3 at{tr == nullptr ? 0.0F : static_cast<float>(tr->px) * to_units,
                                     tr == nullptr ? 0.0F : static_cast<float>(tr->py) * to_units,
                                     0.0F};
                for (std::int64_t i = seen_jumps; i < g.jumps; ++i)
                {
                    CHECK(engine.trigger(jump_event, at) == nullptr);
                    ++r.sounds_fired;
                }
                seen_jumps = g.jumps;
                triggered = true;
            }
            if (g.coin_collected != 0 && seen_coin == 0)
            {
                CHECK(engine.trigger(coin_event, audio::Vec3{}) == nullptr);
                ++r.sounds_fired;
                seen_coin = 1;
                triggered = true;
            }
            if (triggered)
            {
                r.max_voices = std::max(r.max_voices, engine.active_voice_count());
                engine.render_for_test(mix, 128, 48000);
                for (const float sample : mix)
                    if (sample != 0.0F)
                        r.mixed_nonzero = true;
            }
        });

    bool saw_run_state = false;
    for (int t = 0; t < kTicks; ++t)
    {
        const session::TickInputs routed = router.route(s.sim_tick(), plat_raw_for_tick(t));
        for (const session::ActionActivation& a : routed.actions)
            s.inject_action_at(routed.tick, a);
        const session::StepResult sr = s.step(1);
        r.final_sim_tick = sr.sim_tick;
        r.max_live = std::max(r.max_live, parts::particle_count(s.world()));

        anim::AnimatorState animator;
        if (anim::read_animator(s.world(), player, animator) &&
            animator.state == games::kPlatAnimStateRun)
            saw_run_state = true;
    }

    // --- the R-QA-005 session surface: exactly N fixed ticks ran ---------------------------------
    CHECK(r.final_sim_tick == static_cast<std::uint64_t>(kTicks));

    // --- state queries: every landed package visibly did its job ---------------------------------
    const games::PlatformerGame game = games::read_platformer_game(s.world());

    p2::BodyState player_end;
    p2::BodyState crate_end;
    p2::BodyState barrel_end;
    CHECK(p2::read_body(s.world(), player, player_end));
    CHECK(p2::read_body(s.world(), crate, crate_end));
    CHECK(p2::read_body(s.world(), barrel, barrel_end));
    const auto abs_delta = [](std::int64_t from, std::int64_t to)
    {
        const std::int64_t d = to - from;
        return d < 0 ? -d : d;
    };
    r.player_travel = abs_delta(player_spawn.position.x.raw, player_end.position.x.raw);
    CHECK(r.player_travel > (3LL << 16)); // P7: the injected input drove the player over 3 units

    CHECK(game.jumps == 2);                                                    // P2 + P7
    CHECK(game.landings >= 3);                                                 // P2
    CHECK(abs_delta(crate_spawn.position.x.raw, crate_end.position.x.raw) > 0); // P2: crate shoved
    CHECK(abs_delta(barrel_spawn.position.y.raw, barrel_end.position.y.raw) > (1LL << 16));
    CHECK(game.coin_collected == 1); // gameplay: the pickup happened

    parts::EmitterState emitter;
    CHECK(parts::read_emitter(s.world(), puff, emitter));
    r.emitted = emitter.emitted;
    CHECK(r.emitted > 0);  // P4: landing puffs emitted
    CHECK(r.max_live > 0);

    CHECK(saw_run_state); // P3: the sprite anim-graph tracked the player's speed

    CHECK(r.sounds_fired == game.jumps + 1); // P6: jumps + the coin fired on the null backend
    CHECK(r.max_voices >= 1);
    CHECK(r.mixed_nonzero);

    std::printf("[m6-exit-1] platformer-2d: ticks=%llu jumps=%lld coin=%lld emitted=%lld "
                "sounds=%lld\n",
                static_cast<unsigned long long>(r.final_sim_tick),
                static_cast<long long>(game.jumps), static_cast<long long>(game.coin_collected),
                static_cast<long long>(r.emitted), static_cast<long long>(r.sounds_fired));
}
} // namespace

int main()
{
    games::register_roll3d_scenario(CONTEXT_SAMPLES_DIR);
    games::register_platformer2d_scenario(CONTEXT_SAMPLES_DIR);

    run_roll3d();
    run_platformer2d();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("m6-exit-1-games-run: both EXIT-A games ran headless through the session surface "
                "with every landed package active\n");
    return 0;
}
