// M6 exit criterion 4 — `m6-exit-4-netcode-harness` (design §M6-EXIT, issue #197; L-48 /
// R-NET-001): the state-sync harness replicates THE MOVING-BODY SCENE between two sessions on the
// composed-id network identity + the dirty/delta replication metadata, and CONVERGES.
//
// RIDES the landed X2 harness (src/runtime/netsync, PR #191) and the landed EXIT-A roll-3d game
// (issue #194): the SOURCE side is the REAL "roll-3d" RuntimeKernel session — the authority, whose
// physics + injected input genuinely move the replicated bodies — and the REPLICA is a bare kernel
// World that never constructs a physics world. Each fixed tick the source steps, the DirtyScanner
// marks only the bodies whose replicated bytes changed, capture_delta serializes the delta since
// the replica's cursor (keyed by the L-37 composed id + authority), and apply_snapshot converges
// the replica. The harness's own unit gates (netsync-*) prove the mechanism on synthetic scenes;
// THIS gate is the milestone-closing proof over the shipped game's moving-body scene, blocking on
// all three build-matrix legs as the "M6 exit gate" CI step.

#include "sample_games.h"

#include "context/kernel/world.h"
#include "context/packages/input/input_router.h"
#include "context/packages/physics3d/components.h"
#include "context/packages/physics3d/physics_world.h"
#include "context/runtime/netsync/state_sync.h"
#include "context/runtime/session/session.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#ifndef CONTEXT_SAMPLES_DIR
#error "CONTEXT_SAMPLES_DIR (path to the samples/ corpus root) must be defined by the build."
#endif

namespace games = context::tests::games;
namespace session = context::runtime::session;
namespace kernel = context::kernel;
namespace input = context::packages::input;
namespace net = context::runtime::netsync;
namespace p3 = context::packages::physics3d;

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

// Composed-id-shaped network identities (the net id is the opaque L-37 composed id; the harness
// treats it as a uint64 — the netsync convergence gate's established stand-in convention).
constexpr std::uint64_t kBallId = 0x0123456789AB1001ULL;
constexpr std::uint64_t kBoulderAId = 0x0123456789AB1002ULL;
constexpr std::uint64_t kBoulderBId = 0x0123456789AB1003ULL;
constexpr std::uint64_t kFloorId = 0x00000000000000F1ULL;

// The same fixed input schedule the roll-3d gates use — the injected "player" that keeps the
// authority's ball a genuinely MOVING body for the whole replicated run.
std::vector<session::InputEvent> raw_for_tick(int t)
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
} // namespace

int main()
{
    games::register_roll3d_scenario(CONTEXT_SAMPLES_DIR);

    // --- the SOURCE session: the real roll-3d game, the replication authority --------------------
    session::Session src(session::SessionConfig{/*seed=*/42, /*tick_hz=*/60, "roll-3d"});
    const games::RollGame game0 = games::read_roll_game(src.world());
    CHECK(game0.ball_gen != 0);
    const kernel::Entity ball = games::entity_from(game0.ball_index, game0.ball_gen);
    const kernel::Entity boulder_a = games::entity_from(game0.boulder_a_index, game0.boulder_a_gen);
    const kernel::Entity boulder_b = games::entity_from(game0.boulder_b_index, game0.boulder_b_gen);

    // One STATIC scene body too (the arena floor), so the dirty/delta culling assertion below is
    // exercised against a body that genuinely never moves. Found through the physics component
    // set: the floor is the static box whose collider is widest.
    kernel::Entity floor{};
    std::int64_t widest = 0;
    src.world().each<p3::Transform3d, p3::Body3d, p3::Collider3d>(
        [&](kernel::Entity e, p3::Transform3d&, p3::Body3d& body, p3::Collider3d& collider)
        {
            if (body.flags != p3::kBodyFlagDynamic && collider.ex > widest)
            {
                widest = collider.ex;
                floor = e;
            }
        });
    CHECK(widest > 0);

    // The replicated component set: the full physics3d state of the moving-body scene.
    net::ReplicatedComponentSet set;
    set.add<p3::Transform3d>();
    set.add<p3::Velocity3d>();
    set.add<p3::Body3d>();
    set.add<p3::Collider3d>();

    // Register the bodies on the composed-id identity (L-48: metadata bound to the composed id).
    net::NetIdMap src_reg;
    CHECK(net::register_replicated(src.world(), ball, kBallId, /*authority=*/0, src_reg) ==
          nullptr);
    CHECK(net::register_replicated(src.world(), boulder_a, kBoulderAId, 0, src_reg) == nullptr);
    CHECK(net::register_replicated(src.world(), boulder_b, kBoulderBId, 0, src_reg) == nullptr);
    CHECK(net::register_replicated(src.world(), floor, kFloorId, 0, src_reg) == nullptr);
    CHECK(src.world().replication_of(ball) != nullptr);
    CHECK(src.world().replication_of(ball)->net_id == kBallId); // the binding IS the composed id

    // P7: the real routing front-end drives the authority's ball (mirrors the smoke gate).
    input::InputRouter router;
    CHECK(router.install_context(input::InputContext{
              games::kRollContextGameplay,
              input::Layer::Gameplay,
              false,
              {{"keyboard", "A", games::kRollActionMoveX},
               {"keyboard", "D", games::kRollActionMoveX},
               {"keyboard", "W", games::kRollActionMoveY},
               {"keyboard", "S", games::kRollActionMoveY}}}) == nullptr);
    CHECK(router.push_context(games::kRollContextGameplay) == nullptr);

    const std::int64_t ball_spawn_px = src.world().get<p3::Transform3d>(ball)->px;

    // --- the REPLICA: a bare world that never runs physics ---------------------------------------
    kernel::World replica;
    net::NetIdMap rep_map;
    net::DirtyScanner scanner;

    std::uint64_t cursor = 0;
    std::size_t first_delta = 0;
    std::size_t max_after_first = 0;
    bool floor_after_first = false;

    for (int t = 0; t < kTicks; ++t)
    {
        const session::TickInputs routed = router.route(src.sim_tick(), raw_for_tick(t));
        for (const session::ActionActivation& a : routed.actions)
            src.inject_action_at(routed.tick, a);
        const session::StepResult sr = src.step(1);
        CHECK(sr.sim_tick == static_cast<std::uint64_t>(t) + 1); // R-QA-005 simTick

        scanner.scan(src.world(), set); // mark only the bodies whose replicated bytes changed

        const net::StateSyncSnapshot snap = net::capture_delta(src.world(), set, cursor);
        if (t == 0)
            first_delta = snap.entities.size();
        else
        {
            if (snap.entities.size() > max_after_first)
                max_after_first = snap.entities.size();
            for (const net::EntityDelta& d : snap.entities)
                if (d.net_id == kFloorId)
                    floor_after_first = true;
        }

        const net::ApplyResult applied = net::apply_snapshot(replica, snap, set, rep_map);
        CHECK(applied.error == nullptr);
        cursor = snap.source_version;
    }

    // --- convergence: every replicated body's full state is byte-identical on the replica --------
    CHECK(rep_map.size() == 4);
    CHECK(replica.replicated_count() == 4);
    for (const auto& [net_id, src_e] : src_reg)
    {
        const auto it = rep_map.find(net_id);
        CHECK(it != rep_map.end());
        if (it == rep_map.end())
            continue;
        CHECK(replica.has<p3::Transform3d>(it->second));
        CHECK(net::read_replicated_bytes(src.world(), src_e, set) ==
              net::read_replicated_bytes(replica, it->second, set));
        CHECK(replica.replication_of(it->second) != nullptr);
        CHECK(replica.replication_of(it->second)->net_id == net_id); // identity survived the wire
    }

    // --- the scene genuinely MOVED (input-driven), and the replica agrees exactly ----------------
    const std::int64_t ball_end_px = src.world().get<p3::Transform3d>(ball)->px;
    const std::int64_t travel =
        ball_end_px > ball_spawn_px ? ball_end_px - ball_spawn_px : ball_spawn_px - ball_end_px;
    CHECK(travel > (2LL << 16)); // the routed input rolled the ball well over 2 world units
    CHECK(replica.get<p3::Transform3d>(rep_map.at(kBallId))->px == ball_end_px);

    // --- dirty/delta metadata: full snapshot first; the static floor is then always culled -------
    CHECK(first_delta == 4);     // the initial full snapshot carried every replicated body
    CHECK(!floor_after_first);   // the STATIC floor rode only the initial snapshot
    CHECK(max_after_first <= 3); // every later delta is a strict subset

    std::printf("[m6-exit-4] ticks=%d replicated=%zu firstDelta=%zu maxLaterDelta=%zu "
                "ballTravelRaw=%lld\n",
                kTicks, rep_map.size(), first_delta, max_after_first,
                static_cast<long long>(travel));

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("m6-exit-4-netcode-harness: the roll-3d moving-body scene replicated between two "
                "sessions on the composed-id identity and converged\n");
    return 0;
}
