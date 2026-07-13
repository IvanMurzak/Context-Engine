// The platformer-2d runtime scenario mirror (sample_games.h; issue #194): parses the authored
// samples/platformer-2d level layout and registers the game as the session scenario
// "platformer-2d" — the M6 EXIT 2D sample game wired through every landed sim package: input (P7)
// folds mapped actions into the sim InputState, control runs the SAME per-tick run+jump response
// scripts/movement.ts authors in TypeScript (bounded acceleration toward move_x * max-run-speed;
// a grounded jump press becomes an instantaneous take-off), physics2d (P2) steps the platforms +
// the pushable crate, the gameplay system turns fast-fall landings into a deterministic particle
// puff (P4), collects the coin on overlap, and drives the sprite anim-graph parameter from the
// player's horizontal speed (P3); the smoke gate's presentation hook fires the jump/coin sounds on
// the null audio backend (P6, off the sim path). All sim state is integer/fixed-point — the
// scenario is wedge-deterministic by construction (R-SIM-005 / L-54).

#include "sample_games.h"

#include "context/packages/animation/animation_world.h"
#include "context/packages/particles/particle_world.h"
#include "context/packages/physics2d/physics_world.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/vec.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/sim_component.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace context::tests::games
{

namespace session = context::runtime::session;
namespace kernel = context::kernel;
namespace sm = context::packages::simmath;
namespace p2 = context::packages::physics2d;
namespace anim = context::packages::animation;
namespace parts = context::packages::particles;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{

// --- the player's sprite rig: the runtime mirror of player-sprite.anim-graph.json ----------------
// One joint (the sprite anchor), two clips: player_idle_sheet (index 0) and player_run_sheet
// (index 1) — sprite-sheet flipbooks with NO root motion (the entity is moved by the physics body;
// the graph only picks which flipbook plays). idle <-> run gates on the |vx| whole-units parameter
// (threshold kPlatAnimRunThreshold, blend duration 1/8 — the authored values).
anim::Rig make_sprite_rig()
{
    anim::Rig rig;

    rig.skeleton.parents = {-1};
    sm::Transform root;
    root.translation = {kZero, kZero, kZero};
    root.rotation = sm::quat_identity();
    root.scale = {kOne, kOne, kOne};
    rig.skeleton.bind = {root};

    anim::Clip idle_sheet;
    idle_sheet.duration = kOne;
    idle_sheet.loop = true;
    idle_sheet.tracks = {{}};
    idle_sheet.root_velocity = {kZero, kZero, kZero};
    idle_sheet.yaw_rate = kZero;

    anim::Clip run_sheet = idle_sheet; // same shape: a flipbook clip carries no root motion

    rig.clips = {idle_sheet, run_sheet};

    anim::GraphState st_idle; // kPlatAnimStateIdle
    st_idle.clip = 0;
    st_idle.transitions = {anim::Transition{kPlatAnimStateRun, anim::CompareOp::greater_equal,
                                            Fixed::from_int(kPlatAnimRunThreshold),
                                            Fixed::from_ratio(1, 8)}};
    anim::GraphState st_run; // kPlatAnimStateRun
    st_run.clip = 1;
    st_run.transitions = {anim::Transition{kPlatAnimStateIdle, anim::CompareOp::less,
                                           Fixed::from_int(kPlatAnimRunThreshold),
                                           Fixed::from_ratio(1, 8)}};
    rig.graph.states = {st_idle, st_run};
    rig.graph.initial = kPlatAnimStateIdle;

    return rig;
}

// The per-session game state the scenario's systems share (recreated by every factory run).
struct Platformer2dState
{
    p2::PhysicsWorld2d phys2;
    parts::ParticleWorld particles;
    anim::AnimationWorld animation;
};

// The movement.ts tuning constants, Q16 (see samples/platformer-2d/scripts/movement.ts — the
// authored TypeScript and this native mirror MUST stay in lockstep).
const Fixed kMaxRunSpeed = Fixed::from_int(4);          // MAX_RUN_SPEED
const Fixed kRunAccel = Fixed::from_ratio(1, 8);        // RUN_ACCEL
const Fixed kJumpVelocity = Fixed::from_int(6);         // JUMP_VELOCITY
const Fixed kGroundedEpsilon = Fixed::from_ratio(1, 4); // GROUNDED_EPSILON

// A landing is a FAST fall arrested: previously falling faster than 1 unit/s, now no longer
// falling meaningfully. Distinct from resting-contact jitter (which never reaches -1 unit/s).
constexpr std::int64_t kLandingFallRaw = -65536; // -1.0 in Q16

[[nodiscard]] sm::Vec2 fixed_pos2(const NamedPosition& p)
{
    return {Fixed::from_int(p.x), Fixed::from_int(p.y)};
}

[[nodiscard]] bool add_static_box(Platformer2dState& st, kernel::World& w, kernel::Entity e,
                                  sm::Vec2 at, sm::Vec2 half, const char* what)
{
    p2::BodyDesc d;
    d.position = at;
    d.is_static = true;
    d.shape = p2::Shape::Box;
    d.half_extents = half;
    d.friction = Fixed::from_ratio(1, 2);
    d.restitution = Fixed::from_ratio(1, 10);
    const char* err = st.phys2.add_body(w, e, d);
    if (err != nullptr)
        std::fprintf(stderr, "[platformer-2d] add_body(%s) failed: %s\n", what, err);
    return err == nullptr;
}

} // namespace

PlatformerGame read_platformer_game(const kernel::World& world)
{
    return read_singleton<PlatformerGame>(world);
}

void register_platformer2d_scenario(const std::string& samples_dir)
{
    session::register_package_sim_component<PlatformerGame>(
        kPlatformerGameComponentName,
        {"player_index", "player_gen", "crate_index", "crate_gen", "barrel_index", "barrel_gen",
         "puff_index", "puff_gen", "coin_x", "coin_y", "coin_collected", "jumps", "landings",
         "puff_ticks_left", "prev_player_vy"});

    const std::string scene_path = samples_dir + "/platformer-2d/scenes/level-1.scene.json";

    session::register_scenario(
        "platformer-2d",
        [scene_path](session::Session& s) -> std::vector<session::System>
        {
            std::vector<session::System> systems;

            std::vector<NamedPosition> layout;
            if (!load_scene_layout(scene_path, layout))
                return systems;
            const NamedPosition* player_p = find_entity(layout, "Player");
            const NamedPosition* ground_p = find_entity(layout, "Ground");
            const NamedPosition* plat_a_p = find_entity(layout, "Platform-A");
            const NamedPosition* plat_b_p = find_entity(layout, "Platform-B");
            const NamedPosition* crate_p = find_entity(layout, "Crate");
            const NamedPosition* barrel_p = find_entity(layout, "Barrel");
            const NamedPosition* coin_p = find_entity(layout, "Coin");
            const NamedPosition* puff_p = find_entity(layout, "LandingPuff");
            if (player_p == nullptr || ground_p == nullptr || plat_a_p == nullptr ||
                plat_b_p == nullptr || crate_p == nullptr || barrel_p == nullptr ||
                coin_p == nullptr || puff_p == nullptr)
            {
                std::fprintf(stderr, "[platformer-2d] authored level layout is missing an entity\n");
                return systems;
            }

            auto st = std::make_shared<Platformer2dState>();
            kernel::World& w = s.world();

            // The world-singleton InputState the input system folds mapped actions into (P7).
            {
                const kernel::Entity input_singleton = w.create();
                w.add<session::InputState>(input_singleton, session::InputState{});
            }

            // --- physics2d (P2): the ground, the platforms, the player, the crate, the barrel ---
            bool ok = add_static_box(*st, w, w.create(), fixed_pos2(*ground_p),
                                     {Fixed::from_int(24), kOne}, "ground");
            ok = add_static_box(*st, w, w.create(), fixed_pos2(*plat_a_p),
                                {Fixed::from_int(2), kOne}, "platform-a") &&
                 ok;
            ok = add_static_box(*st, w, w.create(), fixed_pos2(*plat_b_p),
                                {Fixed::from_int(2), kOne}, "platform-b") &&
                 ok;

            const kernel::Entity player = w.create();
            {
                p2::BodyDesc d;
                d.position = fixed_pos2(*player_p);
                d.shape = p2::Shape::Circle;
                d.half_extents = {Fixed::from_ratio(1, 2), Fixed::from_ratio(1, 2)};
                d.friction = Fixed::from_ratio(1, 5);
                d.restitution = kZero; // a platformer player lands dead, never bounces
                const char* err = st->phys2.add_body(w, player, d);
                if (err != nullptr)
                {
                    std::fprintf(stderr, "[platformer-2d] add_body(player) failed: %s\n", err);
                    ok = false;
                }
            }
            const kernel::Entity crate = w.create();
            {
                // The pushable crate carries a BOX collider (half-extents (0.5, 0.5) — a 1x1 crate):
                // now that the physics2d narrow phase resolves box-box contacts (issue #199), a box
                // crate settles onto the static box ground and the player genuinely shoves it along
                // the ground (asserted by the smoke gate), reverting the classic round-collider-
                // under-a-crate-sprite trick the v1 narrow phase forced. A 1x1 crate keeps the
                // crate at the player's run height so a run into it is a shove (a 2-unit-tall box
                // would instead be a ledge the player jumps onto). It drops the half unit from its
                // authored center (y == 1) onto the ground (top at y == 0, center rests at 0.5)
                // in the first ticks, before the player arrives.
                p2::BodyDesc d;
                d.position = fixed_pos2(*crate_p);
                d.shape = p2::Shape::Box;
                d.half_extents = {Fixed::from_ratio(1, 2), Fixed::from_ratio(1, 2)};
                d.mass = Fixed::from_int(2);
                d.friction = Fixed::from_ratio(2, 5);
                d.restitution = Fixed::from_ratio(1, 10);
                const char* err = st->phys2.add_body(w, crate, d);
                if (err != nullptr)
                {
                    std::fprintf(stderr, "[platformer-2d] add_body(crate) failed: %s\n", err);
                    ok = false;
                }
            }
            const kernel::Entity barrel = w.create();
            {
                p2::BodyDesc d;
                d.position = fixed_pos2(*barrel_p);
                d.shape = p2::Shape::Circle;
                d.half_extents = {Fixed::from_ratio(1, 2), Fixed::from_ratio(1, 2)};
                d.friction = Fixed::from_ratio(1, 5);
                d.restitution = Fixed::from_ratio(2, 5);
                const char* err = st->phys2.add_body(w, barrel, d);
                if (err != nullptr)
                {
                    std::fprintf(stderr, "[platformer-2d] add_body(barrel) failed: %s\n", err);
                    ok = false;
                }
            }

            // --- animation (P3): the player's sprite anim-graph, mirrored from the authored file
            {
                const char* err = st->animation.set_rig(make_sprite_rig());
                if (err == nullptr)
                    err = st->animation.add_animator(w, player);
                if (err != nullptr)
                {
                    std::fprintf(stderr, "[platformer-2d] animation setup failed: %s\n", err);
                    ok = false;
                }
            }

            // --- particles (P4): the landing-puff emitter, closed (rate 0) until a landing ------
            const kernel::Entity puff = w.create();
            {
                parts::EmitterDesc d;
                d.position = {Fixed::from_int(puff_p->x), Fixed::from_int(puff_p->y),
                              Fixed::from_int(puff_p->z)};
                d.velocity = {kZero, Fixed::from_int(3), kZero};
                d.spread = Fixed::from_ratio(1, 1);
                d.rate = 0;
                d.lifetime = 20;
                d.seed = 0x9142u;
                const char* err = st->particles.add_emitter(w, puff, d);
                if (err != nullptr)
                {
                    std::fprintf(stderr, "[platformer-2d] add_emitter(puff) failed: %s\n", err);
                    ok = false;
                }
            }

            // --- the gameplay singleton: handles + run/jump/landing/pickup bookkeeping ----------
            {
                PlatformerGame g;
                g.player_index = player.index;
                g.player_gen = player.generation;
                g.crate_index = crate.index;
                g.crate_gen = crate.generation;
                g.barrel_index = barrel.index;
                g.barrel_gen = barrel.generation;
                g.puff_index = puff.index;
                g.puff_gen = puff.generation;
                g.coin_x = Fixed::from_int(coin_p->x).raw;
                g.coin_y = Fixed::from_int(coin_p->y).raw;
                w.add<PlatformerGame>(w.create(), g);
            }

            if (!ok)
                return systems; // fail closed: an empty system list fails the smoke gate loudly

            const Fixed dt = Fixed::from_ratio(1, static_cast<std::int64_t>(s.tick_hz()));

            // "input": fold this tick's mapped actions into the InputState singleton (move_x ->
            // the move channel, jump -> the button channel — the authored L-33 action ids).
            systems.push_back(session::System{
                "input",
                [](session::SystemContext& ctx)
                {
                    if (ctx.inputs == nullptr)
                        return;
                    ctx.world.each<session::InputState>(
                        [&](kernel::Entity, session::InputState& state)
                        {
                            for (const session::ActionActivation& a : ctx.inputs->actions)
                            {
                                if (a.action == kPlatActionMoveX)
                                    state.move_x = a.value;
                                else if (a.action == kPlatActionJump)
                                    state.buttons = a.value;
                                else if (a.action == kPlatActionUiMenu)
                                    state.ui += a.value;
                            }
                        });
                }});

            // "control": the movement.ts response — bounded acceleration toward the target run
            // speed + a grounded jump press becoming an instantaneous take-off (P7 → P2).
            systems.push_back(session::System{
                "control",
                [player](session::SystemContext& ctx)
                {
                    std::int64_t move_x = 0;
                    std::int64_t jump = 0;
                    ctx.world.each<session::InputState>(
                        [&](kernel::Entity, session::InputState& state)
                        {
                            move_x = state.move_x;
                            jump = state.buttons;
                        });
                    p2::Velocity2d* vel = ctx.world.get<p2::Velocity2d>(player);
                    PlatformerGame* game = nullptr;
                    ctx.world.each<PlatformerGame>([&](kernel::Entity, PlatformerGame& g)
                                                   { game = &g; });
                    if (vel == nullptr || game == nullptr)
                        return;

                    // Horizontal: step vx toward move_x * kMaxRunSpeed, bounded by kRunAccel.
                    const Fixed target = Fixed::from_int(move_x) * kMaxRunSpeed;
                    Fixed step = target - Fixed::from_raw(vel->vx);
                    if (step > kRunAccel)
                        step = kRunAccel;
                    else if (step < -kRunAccel)
                        step = -kRunAccel;
                    vel->vx += step.raw;

                    // Vertical: a grounded jump press becomes the take-off velocity.
                    const bool grounded = vel->vy > -kGroundedEpsilon.raw &&
                                          vel->vy < kGroundedEpsilon.raw;
                    if (jump != 0 && grounded)
                    {
                        vel->vy = kJumpVelocity.raw;
                        ++game->jumps;
                    }
                }});

            // "physics2d": the P2 stepper (platforms, crate pushes, the barrel on its platform).
            systems.push_back(session::System{
                "physics2d",
                [st, dt](session::SystemContext& ctx)
                {
                    const char* err = st->phys2.step(ctx.world, dt);
                    if (err != nullptr)
                        std::fprintf(stderr, "[platformer-2d] physics2d step failed: %s\n", err);
                }});

            // "gameplay": landings open the puff at the player (P4), the coin collects on overlap
            // (the pickup the presentation hook voices), and the sprite graph's parameter tracks
            // the player's horizontal speed (P3).
            systems.push_back(session::System{
                "gameplay",
                [st, player, puff](session::SystemContext& ctx)
                {
                    PlatformerGame* game = nullptr;
                    ctx.world.each<PlatformerGame>([&](kernel::Entity, PlatformerGame& g)
                                                   { game = &g; });
                    const p2::Velocity2d* vel = ctx.world.get<p2::Velocity2d>(player);
                    const p2::Transform2d* tr = ctx.world.get<p2::Transform2d>(player);
                    parts::ParticleEmitter3d* em = ctx.world.get<parts::ParticleEmitter3d>(puff);
                    if (game == nullptr || vel == nullptr || tr == nullptr || em == nullptr)
                        return;

                    // Landing: a fast fall arrested (never fires on resting-contact jitter).
                    if (game->prev_player_vy < kLandingFallRaw &&
                        vel->vy > -kGroundedEpsilon.raw)
                    {
                        ++game->landings;
                        em->px = tr->px;
                        em->py = tr->py;
                        em->pz = 0;
                        em->rate = 24;
                        game->puff_ticks_left = 2;
                    }
                    else if (game->puff_ticks_left > 0 && --game->puff_ticks_left == 0)
                    {
                        em->rate = 0;
                    }
                    game->prev_player_vy = vel->vy;

                    // Coin pickup: per-axis overlap within 1.25 units, collected exactly once.
                    if (game->coin_collected == 0)
                    {
                        const std::int64_t reach = Fixed::from_ratio(5, 4).raw;
                        std::int64_t dx = tr->px - game->coin_x;
                        std::int64_t dy = tr->py - game->coin_y;
                        dx = dx < 0 ? -dx : dx;
                        dy = dy < 0 ? -dy : dy;
                        if (dx < reach && dy < reach)
                            game->coin_collected = 1;
                    }

                    // Sprite graph parameter: |vx| in whole units drives idle <-> run.
                    std::int64_t speed_raw = vel->vx < 0 ? -vel->vx : vel->vx;
                    const char* err = st->animation.set_param(
                        ctx.world, player, Fixed::from_raw(speed_raw));
                    if (err != nullptr)
                        std::fprintf(stderr, "[platformer-2d] set_param failed: %s\n", err);
                }});

            // "particles": the P4 stepper (puff emission + integration + despawn).
            systems.push_back(session::System{
                "particles",
                [st, dt](session::SystemContext& ctx)
                {
                    const char* err = st->particles.step(ctx.world, dt);
                    if (err != nullptr)
                        std::fprintf(stderr, "[platformer-2d] particles step failed: %s\n", err);
                }});

            // "animation": the P3 stepper (sprite-graph evaluation).
            systems.push_back(session::System{
                "animation",
                [st, dt](session::SystemContext& ctx)
                {
                    const char* err = st->animation.step(ctx.world, dt);
                    if (err != nullptr)
                        std::fprintf(stderr, "[platformer-2d] animation step failed: %s\n", err);
                }});

            return systems;
        });
}

} // namespace context::tests::games
