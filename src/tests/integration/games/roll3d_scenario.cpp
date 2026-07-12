// The roll-3d runtime scenario mirror (sample_games.h; issue #194): parses the authored
// samples/roll-3d layout and registers the game as the session scenario "roll-3d" — the M6 EXIT
// 3D sample game wired through every landed sim package: input (P7) folds mapped actions into the
// sim InputState, control converts held move state into rolling impulses on the Ball, physics3d
// (P1, broad-phased via context_spatial inside the package) steps the bodies, the impact system
// turns ball floor-bounces into a deterministic particle burst (P4) + the flag prop's anim-graph
// parameter (P3), and the smoke gate's presentation hook fires the impact sound on the null audio
// backend (P6, off the sim path). All sim state is integer/fixed-point — the scenario is
// wedge-deterministic by construction (R-SIM-005 / L-54).

#include "sample_games.h"

#include "context/editor/serializer/json_parse.h"
#include "context/packages/animation/animation_world.h"
#include "context/packages/particles/particle_world.h"
#include "context/packages/physics3d/physics_world.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/vec.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/sim_component.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

namespace context::tests::games
{

namespace session = context::runtime::session;
namespace kernel = context::kernel;
namespace serializer = context::editor::serializer;
namespace sm = context::packages::simmath;
namespace p3 = context::packages::physics3d;
namespace anim = context::packages::animation;
namespace parts = context::packages::particles;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{

[[nodiscard]] const serializer::JsonValue* find_member(const serializer::JsonValue& object,
                                                       const char* key)
{
    if (object.type != serializer::JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

[[nodiscard]] std::string read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// --- the flag prop's rig: the runtime mirror of samples/roll-3d/anim/flag-wave.anim-graph.json ---
// Two joints (pole root + flag tip), two clips: flag_idle (index 0, motionless) and flag_wave
// (index 1, a non-zero root yaw rate so waving accumulates sim-visible root motion). The graph
// gates idle <-> wave on the control parameter the impact system couples to the ball's impact
// count (threshold kRollFlagWaveThreshold, blend duration 1/4 — the authored values).
anim::Rig make_flag_rig()
{
    anim::Rig rig;

    rig.skeleton.parents = {-1, 0};
    sm::Transform root;
    root.translation = {kZero, kZero, kZero};
    root.rotation = sm::quat_identity();
    root.scale = {kOne, kOne, kOne};
    sm::Transform tip = root;
    tip.translation = {kZero, Fixed::from_int(2), kZero};
    rig.skeleton.bind = {root, tip};

    anim::Clip flag_idle;
    flag_idle.duration = kOne;
    flag_idle.loop = true;
    flag_idle.tracks = {{}, {}};
    flag_idle.root_velocity = {kZero, kZero, kZero};
    flag_idle.yaw_rate = kZero;

    anim::Clip flag_wave;
    flag_wave.duration = kOne;
    flag_wave.loop = true;
    flag_wave.tracks = {{}, {}};
    flag_wave.root_velocity = {kZero, kZero, kZero};
    flag_wave.yaw_rate = Fixed::from_ratio(1, 8);

    rig.clips = {flag_idle, flag_wave};

    anim::GraphState st_idle; // kRollFlagStateIdle
    st_idle.clip = 0;
    st_idle.transitions = {anim::Transition{kRollFlagStateWave, anim::CompareOp::greater_equal,
                                            Fixed::from_int(kRollFlagWaveThreshold),
                                            Fixed::from_ratio(1, 4)}};
    anim::GraphState st_wave; // kRollFlagStateWave
    st_wave.clip = 1;
    st_wave.transitions = {anim::Transition{kRollFlagStateIdle, anim::CompareOp::less,
                                            Fixed::from_int(kRollFlagWaveThreshold),
                                            Fixed::from_ratio(1, 4)}};
    rig.graph.states = {st_idle, st_wave};
    rig.graph.initial = kRollFlagStateIdle;

    return rig;
}

// The per-session game state the scenario's systems share (recreated by every factory run, so a
// set_seed re-setup gets fresh package worlds — no stale state across rebuilds).
struct Roll3dState
{
    p3::PhysicsWorld3d phys3;
    parts::ParticleWorld particles;
    anim::AnimationWorld animation;
};

[[nodiscard]] bool add_sphere(Roll3dState& st, kernel::World& w, kernel::Entity e, sm::Vec3 at,
                              sm::Vec3 vel, Fixed restitution, Fixed friction)
{
    p3::BodyDesc d;
    d.position = at;
    d.velocity = vel;
    d.restitution = restitution;
    d.friction = friction;
    d.shape = p3::Shape::Sphere;
    d.half_extents = {kOne, kOne, kOne}; // unit radius
    const char* err = st.phys3.add_body(w, e, d);
    if (err != nullptr)
        std::fprintf(stderr, "[roll-3d] add_body(sphere) failed: %s\n", err);
    return err == nullptr;
}

[[nodiscard]] sm::Vec3 fixed_pos(const NamedPosition& p)
{
    return {Fixed::from_int(p.x), Fixed::from_int(p.y), Fixed::from_int(p.z)};
}

} // namespace

bool load_scene_layout(const std::string& scene_path, std::vector<NamedPosition>& out)
{
    out.clear();
    const std::string source = read_file(scene_path);
    if (source.empty())
    {
        std::fprintf(stderr, "[games] cannot read scene '%s'\n", scene_path.c_str());
        return false;
    }
    const serializer::ParseResult parsed = serializer::parse_json(source);
    if (!parsed.ok)
    {
        std::fprintf(stderr, "[games] scene '%s' failed to parse\n", scene_path.c_str());
        return false;
    }
    const serializer::JsonValue* entities = find_member(parsed.root, "entities");
    if (entities == nullptr || entities->type != serializer::JsonValue::Type::array)
    {
        std::fprintf(stderr, "[games] scene '%s' has no entities array\n", scene_path.c_str());
        return false;
    }
    for (const serializer::JsonValue& entity : entities->elements)
    {
        const serializer::JsonValue* name = find_member(entity, "name");
        const serializer::JsonValue* components = find_member(entity, "components");
        if (name == nullptr || name->type != serializer::JsonValue::Type::string ||
            components == nullptr)
            continue;
        const serializer::JsonValue* transform = find_member(*components, "transform");
        if (transform == nullptr)
            continue;
        const serializer::JsonValue* position = find_member(*transform, "position");
        if (position == nullptr || position->type != serializer::JsonValue::Type::array ||
            position->elements.size() != 3)
            continue;
        NamedPosition np;
        np.name = name->string_value;
        std::int64_t* dst[3] = {&np.x, &np.y, &np.z};
        for (std::size_t i = 0; i < 3; ++i)
        {
            const serializer::JsonValue& v = position->elements[i];
            if (v.type != serializer::JsonValue::Type::integer)
            {
                // Game scenes author INTEGER positions by convention (lossless Q16 conversion);
                // anything else is a rotted/ineligible layout — fail closed.
                std::fprintf(stderr,
                             "[games] scene '%s' entity '%s' has a non-integer position "
                             "component (game layouts are integer-authored)\n",
                             scene_path.c_str(), np.name.c_str());
                return false;
            }
            *dst[i] = v.int_value;
        }
        out.push_back(std::move(np));
    }
    return true;
}

const NamedPosition* find_entity(const std::vector<NamedPosition>& layout, const std::string& name)
{
    for (const NamedPosition& p : layout)
        if (p.name == name)
            return &p;
    return nullptr;
}

RollGame read_roll_game(const kernel::World& world)
{
    RollGame out;
    // World::each has no const overload; the walk is read-only, so the cast is benign (the same
    // shape the session's own read-back helpers use).
    kernel::World& mutable_world = const_cast<kernel::World&>(world);
    mutable_world.each<RollGame>([&](kernel::Entity, RollGame& g) { out = g; });
    return out;
}

void register_roll3d_scenario(const std::string& samples_dir)
{
    // The game's own sim component folds into the hierarchical hash by stable name (M6-F0b seam).
    session::register_package_sim_component<RollGame>(
        kRollGameComponentName,
        {"ball_index", "ball_gen", "prop_index", "prop_gen", "burst_index", "burst_gen",
         "boulder_a_index", "boulder_a_gen", "boulder_b_index", "boulder_b_gen", "impacts",
         "burst_ticks_left", "prev_ball_vy"});

    const std::string scene_path = samples_dir + "/roll-3d/scenes/arena.scene.json";

    session::register_scenario(
        "roll-3d",
        [scene_path](session::Session& s) -> std::vector<session::System>
        {
            std::vector<session::System> systems;

            std::vector<NamedPosition> layout;
            if (!load_scene_layout(scene_path, layout))
                return systems;
            const NamedPosition* floor = find_entity(layout, "Floor");
            const NamedPosition* ramp = find_entity(layout, "Ramp");
            const NamedPosition* ball_p = find_entity(layout, "Ball");
            const NamedPosition* boulder_a_p = find_entity(layout, "Boulder-A");
            const NamedPosition* boulder_b_p = find_entity(layout, "Boulder-B");
            const NamedPosition* prop_p = find_entity(layout, "FlagPole");
            const NamedPosition* burst_p = find_entity(layout, "ImpactBurst");
            if (floor == nullptr || ramp == nullptr || ball_p == nullptr ||
                boulder_a_p == nullptr || boulder_b_p == nullptr || prop_p == nullptr ||
                burst_p == nullptr)
            {
                std::fprintf(stderr, "[roll-3d] authored arena layout is missing an entity\n");
                return systems;
            }

            auto st = std::make_shared<Roll3dState>();
            kernel::World& w = s.world();

            // The world-singleton InputState the input system folds mapped actions into (P7).
            {
                const kernel::Entity input_singleton = w.create();
                w.add<session::InputState>(input_singleton, session::InputState{});
            }

            // --- physics3d (P1; the package broad-phases via context_spatial internally) --------
            {
                p3::BodyDesc d;
                d.position = fixed_pos(*floor);
                d.is_static = true;
                d.shape = p3::Shape::Box;
                d.half_extents = {Fixed::from_int(20), kOne, Fixed::from_int(20)};
                d.friction = Fixed::from_ratio(1, 2);
                // A springy arcade floor: the contact solver takes min(a, b) restitution, so the
                // floor's value is what actually gates how bouncy the ball's landings are.
                d.restitution = Fixed::from_ratio(1, 2);
                const char* err = st->phys3.add_body(w, w.create(), d);
                if (err != nullptr)
                    std::fprintf(stderr, "[roll-3d] add_body(floor) failed: %s\n", err);
            }
            {
                p3::BodyDesc d;
                d.position = fixed_pos(*ramp);
                d.orientation =
                    sm::quat_from_axis_angle({kZero, kZero, kOne}, Fixed::from_ratio(-35, 100));
                d.is_static = true;
                d.shape = p3::Shape::Box;
                d.half_extents = {Fixed::from_int(6), Fixed::from_ratio(1, 2), Fixed::from_int(4)};
                d.friction = Fixed::from_ratio(3, 10);
                d.restitution = Fixed::from_ratio(1, 10);
                const char* err = st->phys3.add_body(w, w.create(), d);
                if (err != nullptr)
                    std::fprintf(stderr, "[roll-3d] add_body(ramp) failed: %s\n", err);
            }
            const kernel::Entity ball = w.create();
            const kernel::Entity boulder_a = w.create();
            const kernel::Entity boulder_b = w.create();
            // A bouncy player ball (restitution 3/5): the drop from the authored spawn registers
            // several floor impacts, each driving the burst + sound + the flag's graph parameter.
            bool ok = add_sphere(*st, w, ball, fixed_pos(*ball_p), {kZero, kZero, kZero},
                                 Fixed::from_ratio(3, 5), Fixed::from_ratio(2, 5));
            ok = add_sphere(*st, w, boulder_a, fixed_pos(*boulder_a_p), {-kOne, kZero, kZero},
                            Fixed::from_ratio(3, 5), Fixed::from_ratio(1, 5)) &&
                 ok;
            ok = add_sphere(*st, w, boulder_b, fixed_pos(*boulder_b_p), {kZero, kZero, kZero},
                            Fixed::from_ratio(2, 5), Fixed::from_ratio(3, 10)) &&
                 ok;

            // --- animation (P3): the flag prop, graph mirrored from the authored anim-graph -----
            const kernel::Entity prop = w.create();
            {
                const char* err = st->animation.set_rig(make_flag_rig());
                if (err == nullptr)
                    err = st->animation.add_animator(w, prop);
                if (err != nullptr)
                {
                    std::fprintf(stderr, "[roll-3d] animation setup failed: %s\n", err);
                    ok = false;
                }
            }

            // --- particles (P4): the impact-burst emitter, closed (rate 0) until a bounce -------
            const kernel::Entity burst = w.create();
            {
                parts::EmitterDesc d;
                d.position = fixed_pos(*burst_p);
                d.velocity = {kZero, Fixed::from_int(6), kZero};
                d.spread = Fixed::from_ratio(3, 2);
                d.rate = 0;
                d.lifetime = 30;
                d.seed = 0x9011u;
                const char* err = st->particles.add_emitter(w, burst, d);
                if (err != nullptr)
                {
                    std::fprintf(stderr, "[roll-3d] add_emitter(burst) failed: %s\n", err);
                    ok = false;
                }
            }

            // --- the gameplay singleton: handles + impact/burst bookkeeping (hash-folded) -------
            {
                RollGame g;
                g.ball_index = ball.index;
                g.ball_gen = ball.generation;
                g.prop_index = prop.index;
                g.prop_gen = prop.generation;
                g.burst_index = burst.index;
                g.burst_gen = burst.generation;
                g.boulder_a_index = boulder_a.index;
                g.boulder_a_gen = boulder_a.generation;
                g.boulder_b_index = boulder_b.index;
                g.boulder_b_gen = boulder_b.generation;
                w.add<RollGame>(w.create(), g);
            }

            if (!ok)
                return systems; // fail closed: an empty system list fails the smoke gate loudly

            const Fixed dt = Fixed::from_ratio(1, static_cast<std::int64_t>(s.tick_hz()));
            const Fixed roll_force = Fixed::from_ratio(1, 8);

            // "input": fold this tick's mapped actions into the InputState singleton. The action
            // names are the authored L-33 ids from roll.input-bindings.json (held state persists
            // across ticks with no input, exactly like the demo tenant's fold).
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
                                if (a.action == kRollActionMoveX)
                                    state.move_x = a.value;
                                else if (a.action == kRollActionMoveY)
                                    state.move_y = a.value;
                                else if (a.action == kRollActionUiMenu)
                                    state.ui += a.value;
                            }
                        });
                }});

            // "control": held move state becomes a rolling impulse on the ball (P7 input → force).
            systems.push_back(session::System{
                "control",
                [st, ball, roll_force](session::SystemContext& ctx)
                {
                    std::int64_t move_x = 0;
                    std::int64_t move_y = 0;
                    ctx.world.each<session::InputState>(
                        [&](kernel::Entity, session::InputState& state)
                        {
                            move_x = state.move_x;
                            move_y = state.move_y;
                        });
                    if (move_x == 0 && move_y == 0)
                        return;
                    const sm::Vec3 kick = {Fixed::from_int(move_x) * roll_force, kZero,
                                           Fixed::from_int(move_y) * roll_force};
                    const char* err = st->phys3.apply_impulse(ctx.world, ball, kick);
                    if (err != nullptr)
                        std::fprintf(stderr, "[roll-3d] control impulse failed: %s\n", err);
                }});

            // "physics3d": the P1 stepper (broad-phase prune via context_spatial inside).
            systems.push_back(session::System{
                "physics3d",
                [st, dt](session::SystemContext& ctx)
                {
                    const char* err = st->phys3.step(ctx.world, dt);
                    if (err != nullptr)
                        std::fprintf(stderr, "[roll-3d] physics3d step failed: %s\n", err);
                }});

            // "impact": a ball bounce (vy sign flip up) opens the particle burst at the ball and
            // advances the flag's anim-graph parameter — the P1→P4/P3 gameplay coupling.
            systems.push_back(session::System{
                "impact",
                [st, ball, prop, burst](session::SystemContext& ctx)
                {
                    RollGame* game = nullptr;
                    ctx.world.each<RollGame>([&](kernel::Entity, RollGame& g) { game = &g; });
                    const p3::Velocity3d* vel = ctx.world.get<p3::Velocity3d>(ball);
                    const p3::Transform3d* tr = ctx.world.get<p3::Transform3d>(ball);
                    parts::ParticleEmitter3d* em = ctx.world.get<parts::ParticleEmitter3d>(burst);
                    if (game == nullptr || vel == nullptr || tr == nullptr || em == nullptr)
                        return;
                    if (game->prev_ball_vy < 0 && vel->vy > 0)
                    {
                        ++game->impacts;
                        em->px = tr->px;
                        em->py = tr->py;
                        em->pz = tr->pz;
                        em->rate = 40;
                        game->burst_ticks_left = 2;
                        const char* err = st->animation.set_param(
                            ctx.world, prop, Fixed::from_int(game->impacts));
                        if (err != nullptr)
                            std::fprintf(stderr, "[roll-3d] set_param failed: %s\n", err);
                    }
                    else if (game->burst_ticks_left > 0 && --game->burst_ticks_left == 0)
                    {
                        em->rate = 0;
                    }
                    game->prev_ball_vy = vel->vy;
                }});

            // "particles": the P4 stepper (burst emission + integration + despawn).
            systems.push_back(session::System{
                "particles",
                [st, dt](session::SystemContext& ctx)
                {
                    const char* err = st->particles.step(ctx.world, dt);
                    if (err != nullptr)
                        std::fprintf(stderr, "[roll-3d] particles step failed: %s\n", err);
                }});

            // "animation": the P3 stepper (graph evaluation + root-motion accumulation).
            systems.push_back(session::System{
                "animation",
                [st, dt](session::SystemContext& ctx)
                {
                    const char* err = st->animation.step(ctx.world, dt);
                    if (err != nullptr)
                        std::fprintf(stderr, "[roll-3d] animation step failed: %s\n", err);
                }});

            return systems;
        });
}

} // namespace context::tests::games
