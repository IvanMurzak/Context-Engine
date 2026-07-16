// netsync-packed-replication (M8 task a07, R-NET-001 / L-48) — the M6 state-sync CONVERGENCE harness
// re-run AGAINST PACKS: the M8 wedge build's pack carries the validated L-48/R-NET-001 replication
// metadata, and the harness converges when driven from it.
//
// R-NET-001 binds network ids to the COMPOSED-ENTITY IDENTITY (L-37) — "one identity across sim, saves,
// and net". The M8 wedge build's PACK carries exactly that identity: every packed entity is frozen with
// its composed identity (UnitEntity::identity, docs/chunk-pack-format.md §3.2), and the runtime loads it
// back through the shipped content seam (PackContentSource → RuntimeContentLoader). So the M6 X2
// convergence harness (test_convergence.cpp) — an authority SOURCE session replicating a real moving-
// body scene to a REPLICA that never runs physics, purely over the L-48 dirty/delta metadata — is here
// re-run with the replication net-ids sourced FROM THE PACK, not from test literals:
//   1. build a real v1 pack of a multi-entity scene, load it through the shipped content seam, and read
//      each packed entity's composed identity back out;
//   2. build a source physics scene (a static floor + three dropped dynamic spheres), registering each
//      body for replication under a PACK-CARRIED composed identity as its L-37 net-id;
//   3. replicate source → replica by dirty/delta over N fixed ticks and assert the replica CONVERGES
//      byte-identically, with the dirty/delta culling (full snapshot first, static floor never re-sent)
//      the M6 harness proves — now keyed by the pack's own identities.
// This validates the M8-exit clause "the wedge builds ... carry the validated L-48/R-NET-001 replication
// metadata (the M6 harness re-run against packed builds)".
//
// The physics packages are a TEST dependency (the moving-body sim); context_netsync itself stays
// physics-agnostic. NOT a strict-FP determinism scene — it registers no determinism-* ctest and is
// intentionally absent from the ci.yml `deterministic` job --target list (like netsync-test_convergence);
// it runs on all three build-matrix legs via the CI `build` job's general ctest step (auto-built by
// --preset dev — the `netsync-*` family is not a gate-exclusion prefix).

#include "context/kernel/world.h"
#include "context/packages/physics3d/components.h"
#include "context/packages/physics3d/physics_world.h"
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"
#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/content_source.h"
#include "context/runtime/content/pack_source.h"
#include "context/runtime/netsync/state_sync.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/pack/pack_writer.h"
#include "context/editor/serializer/json_parse.h"

#include "netsync_test.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kernel = context::kernel;
namespace net = context::runtime::netsync;
namespace content = context::runtime::content;
namespace compose = context::editor::compose;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;
namespace p3 = context::packages::physics3d;
namespace sm = context::packages::simmath;

using sm::Fixed;
using sm::kOne;
using sm::kZero;

namespace
{

class MapResolver final : public compose::SceneResolver
{
public:
    bool add(const std::string& path, const char* json)
    {
        serializer::ParseResult parsed = serializer::parse_json(json);
        if (!parsed.ok)
            return false;
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parsed.root);
        if (!doc.has_value())
            return false;
        docs_[path] = std::move(*doc);
        return true;
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// A four-entity wedge scene: the replicated bodies (a floor + three spheres) draw their network
// identities from these entities' composed identities once packed.
const char* kScene = R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "b0b0000000000f10", "name": "Floor", "components": {"transform": {}}},
    {"id": "b0b0000000000501", "name": "Sphere0", "components": {"transform": {}}},
    {"id": "b0b0000000000502", "name": "Sphere1", "components": {"transform": {}}},
    {"id": "b0b0000000000503", "name": "Sphere2", "components": {"transform": {}}}
  ]})";

[[nodiscard]] std::string make_pack()
{
    MapResolver r;
    if (!r.add("scenes/main.scene.json", kScene))
        return {};
    const compose::ComposedScene scene = compose::flatten("scenes/main.scene.json", r);
    if (!scene.ok)
        return {};
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    pack::PackWriteOptions options;
    options.engine_version = 9;
    const pack::PackWriteResult written = pack::write_pack(units, scene, {}, options);
    return written.ok ? written.bytes : std::string{};
}

// Load the pack through the SHIPPED runtime content seam and return every packed entity's composed
// identity (L-37) — the same key R-NET-001 binds network ids to — ascending + deduplicated.
[[nodiscard]] std::vector<std::uint64_t> pack_composed_identities(const std::string& pack_bytes)
{
    std::vector<std::uint64_t> ids;
    content::PackContentSource source(pack_bytes);
    if (!source.ok())
        return ids;
    content::RuntimeContentLoader loader(source);
    for (const content::UnitDescriptor& u : source.directory())
    {
        if (!loader.load_now(u.unit_id))
            return {};
        const content::LoadedUnit* lu = loader.resident_unit(u.unit_id);
        if (lu == nullptr)
            return {};
        for (const content::UnitEntity& e : lu->entities)
            ids.push_back(e.identity);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

} // namespace

int main()
{
    // --- the packed wedge build carries the L-37 composed identities R-NET-001 binds net-ids to -----
    const std::string pack_bytes = make_pack();
    CHECK(!pack_bytes.empty());
    const std::vector<std::uint64_t> pack_ids = pack_composed_identities(pack_bytes);
    CHECK(pack_ids.size() >= 4);       // four packed entities -> four distinct composed identities
    for (const std::uint64_t id : pack_ids)
        CHECK(id != 0);                // a composed identity is never the invalid (zero) net-id

    // The four replicated net-ids are the pack's own composed identities (not test literals).
    const std::uint64_t floor_id = pack_ids[0];
    const std::uint64_t sphere_ids[] = {pack_ids[1], pack_ids[2], pack_ids[3]};

    // --- source authority: a real moving-body physics scene keyed by the PACK identities ------------
    kernel::World src;
    p3::PhysicsWorld3d phys;
    net::NetIdMap src_reg;

    net::ReplicatedComponentSet set;
    set.add<p3::Transform3d>();
    set.add<p3::Velocity3d>();
    set.add<p3::Body3d>();
    set.add<p3::Collider3d>();

    {
        p3::BodyDesc floor;
        floor.position = {kZero, Fixed::from_int(-1), kZero};
        floor.is_static = true;
        floor.shape = p3::Shape::Box;
        floor.half_extents = {Fixed::from_int(20), kOne, Fixed::from_int(20)};
        floor.friction = Fixed::from_ratio(1, 2);
        floor.restitution = Fixed::from_ratio(1, 5);
        const kernel::Entity e = src.create();
        CHECK(phys.add_body(src, e, floor) == nullptr);
        CHECK(net::register_replicated(src, e, floor_id, /*authority*/ 0, src_reg) == nullptr);
    }
    const Fixed drop_y[] = {Fixed::from_int(6), Fixed::from_int(8), Fixed::from_int(5)};
    const Fixed vx0[] = {kOne, -kOne, kZero};
    for (int i = 0; i < 3; ++i)
    {
        p3::BodyDesc s;
        s.position = {Fixed::from_int(i * 2 - 2), drop_y[i], kZero};
        s.velocity = {vx0[i], kZero, kZero};
        s.restitution = Fixed::from_ratio(2, 5);
        s.friction = Fixed::from_ratio(3, 10);
        const kernel::Entity e = src.create();
        CHECK(phys.add_body(src, e, s) == nullptr);
        CHECK(net::register_replicated(src, e, sphere_ids[i], /*authority*/ 0, src_reg) == nullptr);
    }

    std::vector<std::int64_t> initial_py;
    for (const std::uint64_t id : sphere_ids)
        initial_py.push_back(src.get<p3::Transform3d>(src_reg.at(id))->py);

    // --- replicate source -> replica by dirty/delta over N fixed ticks ------------------------------
    kernel::World replica; // NEVER runs physics — it only receives deltas.
    net::NetIdMap rep_map;
    net::DirtyScanner scanner;

    constexpr int kTicks = 150;
    const Fixed kDt = Fixed::from_ratio(1, 60);
    std::uint64_t cursor = 0;
    std::size_t first_delta = 0;
    std::size_t max_after_first = 0;
    bool floor_after_first = false;

    for (int t = 0; t < kTicks; ++t)
    {
        CHECK(phys.step(src, kDt) == nullptr);
        scanner.scan(src, set);
        const net::StateSyncSnapshot snap = net::capture_delta(src, set, cursor);
        if (t == 0)
            first_delta = snap.entities.size();
        else
        {
            if (snap.entities.size() > max_after_first)
                max_after_first = snap.entities.size();
            for (const net::EntityDelta& d : snap.entities)
                if (d.net_id == floor_id)
                    floor_after_first = true;
        }
        const net::ApplyResult r = net::apply_snapshot(replica, snap, set, rep_map);
        CHECK(r.error == nullptr);
        cursor = snap.source_version;
    }

    // --- convergence: every replicated body's full state is byte-identical on the replica -----------
    CHECK(rep_map.size() == 4);
    CHECK(replica.replicated_count() == 4);
    for (const auto& [net_id, src_e] : src_reg)
    {
        const kernel::Entity rep_e = rep_map.at(net_id);
        CHECK(replica.has<p3::Transform3d>(rep_e));
        CHECK(net::read_replicated_bytes(src, src_e, set) ==
              net::read_replicated_bytes(replica, rep_e, set));
    }

    // --- every replication net-id came from the PACK's composed identities (R-NET-001 binding) ------
    for (const auto& [net_id, src_e] : src_reg)
    {
        static_cast<void>(src_e);
        CHECK(std::find(pack_ids.begin(), pack_ids.end(), net_id) != pack_ids.end());
        CHECK(rep_map.find(net_id) != rep_map.end());
    }

    // --- non-vacuous: at least one sphere actually moved, and never tunneled the floor --------------
    bool any_moved = false;
    for (int i = 0; i < 3; ++i)
    {
        const kernel::Entity src_e = src_reg.at(sphere_ids[i]);
        const std::int64_t final_py = src.get<p3::Transform3d>(src_e)->py;
        if (final_py != initial_py[i])
            any_moved = true;
        CHECK(final_py > -sm::kFixedOneRaw); // no tunneling
        CHECK(replica.get<p3::Transform3d>(rep_map.at(sphere_ids[i]))->py == final_py);
    }
    CHECK(any_moved);

    // --- dirty/delta culling: full snapshot first, static floor never re-sent -----------------------
    CHECK(first_delta == 4);     // the initial full snapshot carried every replicated body
    CHECK(!floor_after_first);   // the STATIC floor rode only the initial snapshot
    CHECK(max_after_first <= 3); // every later delta is a strict subset (floor always culled)

    std::printf("[netsync-packed-replication] packIds=%zu floorId=0x%016llX converged=4\n",
                pack_ids.size(), static_cast<unsigned long long>(floor_id));

    NETSYNC_TEST_MAIN_END();
}
