// The R-NET-001 two-session state-sync CONVERGENCE harness (M6 X2) — the DoD centerpiece.
//
// A REAL moving-body scene (dynamic context_physics3d, then context_physics2d, bodies) is replicated
// from a SOURCE session (the authority, which runs the physics) to a REPLICA session (which never
// runs physics) purely through the L-48 dirty/delta replication metadata: each tick the source steps
// the sim, the DirtyScanner marks the bodies whose replicated state changed, capture_delta serializes
// the delta since the replica's cursor (keyed by the L-37 composed id + authority), and apply_snapshot
// applies it to the replica. After N fixed ticks the replica CONVERGES — its bodies' positions /
// velocities are byte-identical to the source's — proving the L-48 hooks carry enough state to
// replicate a moving scene (R-NET-001, validated in v1 by an in-process two-session harness).
//
// It also exercises: DIRTY/DELTA culling (a static floor rides only the initial full snapshot and is
// never re-sent; settled bodies drop out of the delta), and an AUTHORITY handover mid-run (the L-48
// authority field replicates). This is a CONVERGENCE / netcode test, NOT a strict-FP determinism
// scene — it registers no determinism-* ctest and drives no new determinism gate.

#include "context/kernel/world.h"
#include "context/packages/physics2d/components.h"
#include "context/packages/physics2d/physics_world.h"
#include "context/packages/physics3d/components.h"
#include "context/packages/physics3d/physics_world.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"
#include "context/runtime/netsync/state_sync.h"
#include "netsync_test.h"

#include <cstdint>
#include <vector>

namespace kernel = context::kernel;
namespace net = context::runtime::netsync;
namespace p3 = context::packages::physics3d;
namespace p2 = context::packages::physics2d;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{
// A composed-id-shaped net identity (the net id is the opaque L-37 composed id; state_sync treats it
// as a uint64). These 16-hex values stand in for real composed identities in this in-process harness.
constexpr std::uint64_t kFloorId = 0x00000000000000F1ULL;
constexpr std::uint64_t kSphere0 = 0x0123456789AB0001ULL;
constexpr std::uint64_t kSphere1 = 0x0123456789AB0002ULL;
constexpr std::uint64_t kSphere2 = 0x0123456789AB0003ULL;

const Fixed kDt = Fixed::from_ratio(1, 60);

// ---- physics3d: a static floor + three dropped dynamic spheres ---------------------------------
void run_convergence_3d()
{
    using namespace context::packages::physics3d;

    kernel::World src;
    PhysicsWorld3d phys;
    net::NetIdMap src_reg;

    net::ReplicatedComponentSet set;
    set.add<Transform3d>();
    set.add<Velocity3d>();
    set.add<Body3d>();
    set.add<Collider3d>();

    // Floor: a large static slab, top surface at y == 0.
    {
        BodyDesc floor;
        floor.position = {kZero, Fixed::from_int(-1), kZero};
        floor.is_static = true;
        floor.shape = Shape::Box;
        floor.half_extents = {Fixed::from_int(20), kOne, Fixed::from_int(20)};
        floor.friction = Fixed::from_ratio(1, 2);
        floor.restitution = Fixed::from_ratio(1, 5);
        const kernel::Entity e = src.create();
        CHECK(phys.add_body(src, e, floor) == nullptr);
        CHECK(net::register_replicated(src, e, kFloorId, /*authority*/ 0, src_reg) == nullptr);
    }
    // Three dynamic unit spheres dropped from distinct heights with initial horizontal velocity.
    const std::uint64_t sphere_ids[] = {kSphere0, kSphere1, kSphere2};
    const Fixed drop_y[] = {Fixed::from_int(6), Fixed::from_int(8), Fixed::from_int(5)};
    const Fixed vx0[] = {kOne, -kOne, kZero};
    for (int i = 0; i < 3; ++i)
    {
        BodyDesc s;
        s.position = {Fixed::from_int(i * 2 - 2), drop_y[i], kZero};
        s.velocity = {vx0[i], kZero, kZero};
        s.restitution = Fixed::from_ratio(2, 5);
        s.friction = Fixed::from_ratio(3, 10);
        const kernel::Entity e = src.create();
        CHECK(phys.add_body(src, e, s) == nullptr);
        CHECK(net::register_replicated(src, e, sphere_ids[i], /*authority*/ 0, src_reg) == nullptr);
    }

    // Record each dynamic sphere's initial height so we can prove the scene really moved.
    std::vector<std::int64_t> initial_py;
    for (const std::uint64_t id : sphere_ids)
        initial_py.push_back(src.get<Transform3d>(src_reg.at(id))->py);

    kernel::World replica; // NEVER runs physics — it only receives deltas.
    net::NetIdMap rep_map;
    net::DirtyScanner scanner;

    constexpr int kTicks = 150;
    std::uint64_t cursor = 0;
    std::size_t first_delta = 0;
    std::size_t max_after_first = 0;
    bool floor_after_first = false;

    for (int t = 0; t < kTicks; ++t)
    {
        CHECK(phys.step(src, kDt) == nullptr);
        scanner.scan(src, set); // mark only the bodies whose replicated bytes changed

        // Hand authority over sphere 0 from peer 0 -> 2 mid-run; the L-48 handover must replicate.
        if (t == 40)
            CHECK(src.set_replication_authority(src_reg.at(kSphere0), 2));

        const net::StateSyncSnapshot snap = net::capture_delta(src, set, cursor);
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

        const net::ApplyResult r = net::apply_snapshot(replica, snap, set, rep_map);
        CHECK(r.error == nullptr);
        cursor = snap.source_version;
    }

    // --- convergence: every replicated body's full state is byte-identical on the replica ----------
    CHECK(rep_map.size() == 4);
    CHECK(replica.replicated_count() == 4);
    for (const auto& [net_id, src_e] : src_reg)
    {
        const kernel::Entity rep_e = rep_map.at(net_id);
        CHECK(replica.has<Transform3d>(rep_e));
        CHECK(replica.has<Velocity3d>(rep_e));
        CHECK(net::read_replicated_bytes(src, src_e, set) ==
              net::read_replicated_bytes(replica, rep_e, set));
    }

    // --- non-vacuous: at least one sphere actually moved, and never tunneled the floor ------------
    bool any_moved = false;
    for (int i = 0; i < 3; ++i)
    {
        const kernel::Entity src_e = src_reg.at(sphere_ids[i]);
        const std::int64_t final_py = src.get<Transform3d>(src_e)->py;
        if (final_py != initial_py[i])
            any_moved = true;
        CHECK(final_py > -sm::kFixedOneRaw);                                 // no tunneling
        CHECK(replica.get<Transform3d>(rep_map.at(sphere_ids[i]))->py == final_py); // replica agrees
    }
    CHECK(any_moved);

    // --- dirty/delta culling: full snapshot first, static floor never re-sent ---------------------
    CHECK(first_delta == 4);          // the initial full snapshot carried every replicated body
    CHECK(!floor_after_first);        // the STATIC floor rode only the initial snapshot
    CHECK(max_after_first <= 3);      // every later delta is a strict subset (floor always culled)

    // --- the authority handover replicated to the replica -----------------------------------------
    CHECK(src.replication_of(src_reg.at(kSphere0))->authority == 2);
    CHECK(replica.replication_of(rep_map.at(kSphere0))->authority == 2);
}

// ---- physics2d: a static floor + two dropped dynamic circles (symmetric confirmation) ----------
void run_convergence_2d()
{
    using namespace context::packages::physics2d;

    kernel::World src;
    PhysicsWorld2d phys;
    net::NetIdMap src_reg;

    net::ReplicatedComponentSet set;
    set.add<Transform2d>();
    set.add<Velocity2d>();
    set.add<Body2d>();
    set.add<Collider2d>();

    {
        BodyDesc floor;
        floor.position = {kZero, Fixed::from_int(-1)};
        floor.is_static = true;
        floor.shape = Shape::Box;
        floor.half_extents = {Fixed::from_int(20), kOne};
        floor.friction = Fixed::from_ratio(1, 2);
        floor.restitution = Fixed::from_ratio(1, 5);
        const kernel::Entity e = src.create();
        CHECK(phys.add_body(src, e, floor) == nullptr);
        CHECK(net::register_replicated(src, e, kFloorId, 0, src_reg) == nullptr);
    }
    const std::uint64_t circle_ids[] = {kSphere0, kSphere1};
    for (int i = 0; i < 2; ++i)
    {
        BodyDesc c;
        c.position = {Fixed::from_int(i * 2 - 1), Fixed::from_int(6 + i)};
        c.velocity = {(i == 0 ? kOne : -kOne), kZero};
        c.restitution = Fixed::from_ratio(2, 5);
        c.friction = Fixed::from_ratio(3, 10);
        const kernel::Entity e = src.create();
        CHECK(phys.add_body(src, e, c) == nullptr);
        CHECK(net::register_replicated(src, e, circle_ids[i], 0, src_reg) == nullptr);
    }

    kernel::World replica;
    net::NetIdMap rep_map;
    net::DirtyScanner scanner;

    constexpr int kTicks = 120;
    std::uint64_t cursor = 0;
    bool any_moved = false;
    std::int64_t init_py0 = src.get<Transform2d>(src_reg.at(kSphere0))->py;

    for (int t = 0; t < kTicks; ++t)
    {
        CHECK(phys.step(src, kDt) == nullptr);
        scanner.scan(src, set);
        const net::StateSyncSnapshot snap = net::capture_delta(src, set, cursor);
        CHECK(net::apply_snapshot(replica, snap, set, rep_map).error == nullptr);
        cursor = snap.source_version;
    }

    CHECK(rep_map.size() == 3);
    for (const auto& [net_id, src_e] : src_reg)
    {
        const kernel::Entity rep_e = rep_map.at(net_id);
        CHECK(net::read_replicated_bytes(src, src_e, set) ==
              net::read_replicated_bytes(replica, rep_e, set));
    }
    if (src.get<Transform2d>(src_reg.at(kSphere0))->py != init_py0)
        any_moved = true;
    CHECK(any_moved);
    // The replica converged without ever constructing a PhysicsWorld2d.
    CHECK(replica.get<Transform2d>(rep_map.at(kSphere0))->py ==
          src.get<Transform2d>(src_reg.at(kSphere0))->py);
}
} // namespace

int main()
{
    run_convergence_3d();
    run_convergence_2d();
    NETSYNC_TEST_MAIN_END();
}
