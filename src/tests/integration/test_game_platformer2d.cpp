// The platformer-2d SAMPLE-GAME SMOKE GATE (M6 EXIT-A, issue #194; R-QA-005 / R-QA-013 /
// R-HEAD-002) — the per-game headless proof the EXIT-B m6-exit-1 gate rides: the authored (and now
// EXTENDED) samples/platformer-2d project builds through the REAL RuntimeKernel session surface
// (the "platformer-2d" registered scenario parses the authored level-1 layout) and headless-steps
// N fixed ticks with NO GPU and NO audio device — while every landed gameplay package visibly does
// its job:
//   * P7 input   — raw keyboard events routed through the REAL context_input router (contexts
//                  mirroring player.input-bindings.json) into the session's injection sink; the
//                  mapped run/jump actions drive the player;
//   * P2 physics — the player runs and jumps on the ground, SHOVES the pushable crate, and the
//                  barrel exercises Platform-A;
//   * P4 particles — each landing opens a deterministic particle puff;
//   * P3 animation — the sprite anim-graph transitions idle -> run with the player's speed;
//   * P6 audio   — jumps + the coin pickup trigger the authored events on miniaudio's NULL
//                  backend (presentation observer, OFF the sim path — R-SIM-001).
// The player control is the native mirror of the authored TypeScript (scripts/movement.ts).
// Progress is asserted against the R-QA-005 simTick counter + state queries, then within-run
// determinism and the CROSS-PLATFORM golden (integer/fixed-point sim + fixed-endian fold =>
// portable digests, the L-54 wedge law).
//
// Updating the goldens: they change only when the scene, a package stepper, or the input schedule
// changes ON PURPOSE. Re-derive by running this gate — it prints the observed values — then paste.

#include "sample_games.h"

#include "context/packages/animation/animation_world.h"
#include "context/packages/audio/audio_engine.h"
#include "context/packages/input/input_router.h"
#include "context/packages/particles/particle_world.h"
#include "context/packages/physics2d/physics_world.h"
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

// The fixed input schedule (the recorded "player"): settle the spawn drop, run right (D held),
// tap jump twice mid-run (Space press + release), then release and coast. The rightward run
// shoves the crate and crosses the coin.
std::vector<session::InputEvent> raw_for_tick(int t)
{
    std::vector<session::InputEvent> raw;
    if (t == 10)
        raw.push_back(session::InputEvent{"keyboard", "D", 1});
    else if (t == 60)
        raw.push_back(session::InputEvent{"keyboard", "Space", 1}); // jump tap (grounded)
    else if (t == 61)
        raw.push_back(session::InputEvent{"keyboard", "Space", 0});
    else if (t == 140)
        raw.push_back(session::InputEvent{"keyboard", "Space", 1}); // held: fires on touch-down
    else if (t == 150)
        raw.push_back(session::InputEvent{"keyboard", "Space", 0});
    else if (t == 200)
        raw.push_back(session::InputEvent{"keyboard", "D", 0}); // release; coast to the end
    return raw;
}

// Install the router contexts mirroring samples/platformer-2d/input/player.input-bindings.json.
void install_contexts(input::InputRouter& router)
{
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
}

struct RunResult
{
    std::uint64_t final_sim_tick = 0;
    std::uint64_t final_root = 0;
    std::uint64_t trace_fold = 0;

    // Activity read-back (proves every package genuinely did its job).
    std::int64_t jumps = 0;
    std::int64_t landings = 0;
    std::int64_t coin_collected = 0;
    std::int64_t player_travel_x = 0; // |dx| of the player from its authored spawn (raw Q16)
    std::int64_t crate_travel = 0;    // |dx| of the crate (the shove proof)
    std::int64_t barrel_drop = 0;     // |dy| of the barrel (it fell onto Platform-A)
    std::int64_t emitted = 0;         // puff emitter's running emission count
    std::size_t max_live = 0;         // peak live particle count over the run
    bool saw_run_state = false;       // the sprite graph reached the run state mid-run
    int final_anim_state = -1;
    std::int64_t sounds_fired = 0; // jump + coin sounds triggered on the null backend
    std::size_t max_voices = 0;
    bool mixed_nonzero = false;
};

RunResult run_fixture()
{
    RunResult r;

    std::vector<games::NamedPosition> layout;
    CHECK(games::load_scene_layout(std::string(CONTEXT_SAMPLES_DIR) +
                                       "/platformer-2d/scenes/level-1.scene.json",
                                   layout));
    const games::NamedPosition* camera = games::find_entity(layout, "MainCamera");
    CHECK(camera != nullptr);

    // The REAL session surface: the registered "platformer-2d" scenario builds the game world.
    session::Session s(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "platformer-2d"});
    CHECK(s.system_names().size() == 6); // input, control, physics2d, gameplay, particles, animation

    const games::PlatformerGame game0 = games::read_platformer_game(s.world());
    CHECK(game0.player_gen != 0); // the scenario factory built the world (fail-closed otherwise)
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

    // P7: the REAL routing front-end, contexts mirroring the authored bindings.
    input::InputRouter router;
    install_contexts(router);

    // P6: the null-backend audio engine, bus graph mirroring game-mix.audio-bus.json plus the
    // jump/coin EventDescs mirroring the authored events. Entirely OFF the sim path.
    audio::AudioEngine engine;
    CHECK(engine.init() == nullptr);
    CHECK(engine.configure_buses({audio::BusConfig{"66f78214eb2942ad", 1.0F, -1},
                                  audio::BusConfig{"5a08393cca219c68", 0.9F, 0}}) == nullptr);
    engine.set_listener(audio::Vec3{static_cast<float>(camera->x),
                                    static_cast<float>(camera->y),
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
    coin_event.spatial = false; // the authored pickup jingle is non-spatial
    coin_event.life_seconds = 0.25F;

    // The presentation hook (the R-HEAD-002 inter-tick service point): voice each newly recorded
    // jump at the player's position, and the coin pickup once. Reads sim state, never writes it.
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

        anim::AnimatorState animator;
        if (anim::read_animator(s.world(), player, animator) &&
            animator.state == games::kPlatAnimStateRun)
            r.saw_run_state = true;
    }
    r.final_root = s.state_hash().root;
    r.trace_fold = fold.digest();

    // --- read back the activity ------------------------------------------------------------------
    const games::PlatformerGame game = games::read_platformer_game(s.world());
    r.jumps = game.jumps;
    r.landings = game.landings;
    r.coin_collected = game.coin_collected;

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
    r.player_travel_x = abs_delta(player_spawn.position.x.raw, player_end.position.x.raw);
    r.crate_travel = abs_delta(crate_spawn.position.x.raw, crate_end.position.x.raw);
    r.barrel_drop = abs_delta(barrel_spawn.position.y.raw, barrel_end.position.y.raw);

    parts::EmitterState emitter;
    CHECK(parts::read_emitter(s.world(), puff, emitter));
    r.emitted = emitter.emitted;

    anim::AnimatorState animator;
    CHECK(anim::read_animator(s.world(), player, animator));
    r.final_anim_state = animator.state;

    return r;
}

// The golden digests, derived on the reference build and asserted identical on every matrix
// platform (Linux-x64 / Win-x64 / macOS-ARM64).
constexpr std::uint64_t kGoldenFinalRoot = 0x93EF5B50C4D8FECFULL;
constexpr std::uint64_t kGoldenTraceFold = 0x92174A3C237700BCULL;
} // namespace

int main()
{
    games::register_platformer2d_scenario(CONTEXT_SAMPLES_DIR);

    const RunResult a = run_fixture();

    std::printf("[game-smoke-platformer-2d] ticks=%llu jumps=%lld landings=%lld coin=%lld "
                "crateTravel=%lld emitted=%lld maxLive=%zu sounds=%lld finalRoot=0x%016llX "
                "traceFold=0x%016llX\n",
                static_cast<unsigned long long>(a.final_sim_tick),
                static_cast<long long>(a.jumps), static_cast<long long>(a.landings),
                static_cast<long long>(a.coin_collected), static_cast<long long>(a.crate_travel),
                static_cast<long long>(a.emitted), a.max_live,
                static_cast<long long>(a.sounds_fired),
                static_cast<unsigned long long>(a.final_root),
                static_cast<unsigned long long>(a.trace_fold));

    // --- the session surface: exactly N fixed ticks ran (R-QA-005 simTick) -----------------------
    CHECK(a.final_sim_tick == static_cast<std::uint64_t>(kTicks));

    // --- P7: the routed input actually drove the player ------------------------------------------
    CHECK(a.player_travel_x > (3LL << 16)); // the player ran well over 3 world units

    // --- P2: platform physics is genuinely active ------------------------------------------------
    CHECK(a.jumps == 2);            // both jump taps converted (grounded at each press)
    CHECK(a.landings >= 3);         // the spawn drop + both jump landings
    CHECK(a.crate_travel > 0);      // the run shoved the pushable crate
    CHECK(a.barrel_drop > (1LL << 16)); // the barrel fell onto Platform-A

    // --- P4: the landing puffs emitted (and some particles were alive mid-run) -------------------
    CHECK(a.emitted > 0);
    CHECK(a.max_live > 0);

    // --- P3: the sprite anim-graph tracked the player's speed ------------------------------------
    CHECK(a.saw_run_state); // idle -> run while running

    // --- gameplay: the coin pickup happened -------------------------------------------------------
    CHECK(a.coin_collected == 1);

    // --- P6: jump + coin sounds fired on the NULL backend ----------------------------------------
    CHECK(a.sounds_fired == a.jumps + 1);
    CHECK(a.max_voices >= 1);
    CHECK(a.mixed_nonzero);

    // --- within-run determinism: a second identical run reproduces every digest exactly ----------
    const RunResult b = run_fixture();
    CHECK(b.final_root == a.final_root);
    CHECK(b.trace_fold == a.trace_fold);
    CHECK(b.jumps == a.jumps);
    CHECK(b.coin_collected == a.coin_collected);

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 -----
    CHECK(a.final_root == kGoldenFinalRoot);
    CHECK(a.trace_fold == kGoldenTraceFold);

    return g_failures == 0 ? 0 : 1;
}
