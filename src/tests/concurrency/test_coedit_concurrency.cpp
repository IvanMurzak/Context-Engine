// L-50 multi-client CO-EDIT concurrency validation, Scenario 1 (issue #284, a16-l50; R-COLLAB-001/002,
// L-30, R-QA-010, R-QA-011). Red-teams the EXISTING M1 concurrency machinery end-to-end — it adds no
// production code, it drives the shipped surfaces:
//   * the L-30 rebase-or-drop gesture-commit engine (commit_override_write) under a concurrent SECOND
//     client — REBASE when the co-writer touched an UNRELATED field, DROP LOUDLY (never a silent
//     overwrite) when it touched THIS field path;
//   * CAS `--if-match` — the raw-byte content hash (filesync::content_hash) the write path guards on;
//   * the R-QA-010 fault seams (crash points / watcher loss-dup-reorder / slow-client overflow) as a
//     COMPOSITION check that the co-edit safety net's substrate converges (the exhaustive 512-seed
//     per-mechanism sweeps live in the dedicated testing- tests — this validates them composed);
//   * a 10,000-round SEEDED property run asserting the L-50 guarantee: NO LOST UPDATE — every
//     acknowledged write is present, and a colliding write is loudly refused, never silently clobbered.
//
// In-process over the real seams is the deterministic, cross-OS-portable way to drive this: the M1
// daemon is serial-single-connection (kernel_server.h), so "multi-client" co-edit safety is CAS +
// write-queue serialization + rebase-or-drop, NOT socket multiplexing (L-50). The honest lost-update
// SIGNAL is the `cas.mismatch` catalog code + the loud L-30 `dropped` status — there is no bespoke
// concurrent-write event enum in M1, so this validates the guarantee at the mechanism that carries it.

#include "concurrency_test.h"

#include "context/editor/filesync/content_hash.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/serializer/json_tree.h"
#include "context/testing/fault_scenarios.h"
#include "context/testing/seeded_rng.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

using conctest::jint;
using conctest::json_eq;
using conctest::jstr;
using context::editor::filesync::content_hash;
using context::editor::serializer::JsonValue;
using context::testing::SeededRng;
namespace inspector = context::editor::gui::panels::inspector;

namespace
{

// An in-memory model of ONE authored file two clients contend on: a raw-byte CAS token (file_hash) +
// the current composed value at each field pointer. It IS the `context set` write path the L-30 engine
// commits through (OverrideWriteGateway). A concurrent SECOND client is modeled by `on_first_attempt`,
// fired once inside the FIRST attempt() before its CAS check — exactly the M5 inspector-panel test
// pattern, reused here to drive commit_override_write directly.
class CoeditGateway final : public inspector::OverrideWriteGateway
{
public:
    mutable std::uint64_t file_hash = 1;
    mutable std::map<std::string, JsonValue> fields;
    std::function<void()> on_first_attempt;
    mutable bool fired = false;

    inspector::WriteAttempt attempt(const inspector::OverrideWriteRequest& request,
                                    std::uint64_t expected_raw_hash) const override
    {
        if (on_first_attempt && !fired)
        {
            fired = true;
            on_first_attempt();
        }
        inspector::WriteAttempt a;
        if (expected_raw_hash == file_hash)
        {
            file_hash += 1; // the write advances the file's raw bytes (a new CAS token)
            fields[request.pointer] = request.value;
            a.applied = true;
            a.file = request.root_scene;
            a.pointer = request.pointer;
            a.raw_hash = file_hash;
        }
        else
        {
            a.cas_mismatch = true;
            a.code = "cas.mismatch";
            a.raw_hash = file_hash; // the CURRENT token (what the L-30 rebase retries on)
        }
        return a;
    }

    inspector::FieldState read(const std::string&, const std::vector<std::string>&,
                              const std::string& pointer) const override
    {
        inspector::FieldState s;
        s.present = true;
        s.raw_hash = file_hash;
        const auto it = fields.find(pointer);
        if (it != fields.end())
            s.value = it->second;
        return s;
    }
};

inspector::OverrideWriteRequest make_request(const std::string& scene,
                                             const std::vector<std::string>& id_path,
                                             const std::string& pointer, JsonValue value)
{
    inspector::OverrideWriteRequest r;
    r.root_scene = scene;
    r.id_path = id_path;
    r.pointer = pointer;
    r.value = std::move(value);
    return r;
}

// The 10k-round property core, factored so the sweep AND the pinned regression seeds replay it.
struct PropStats
{
    std::uint64_t applied = 0;  // no concurrent writer -> the gesture landed
    std::uint64_t rebased = 0;  // co-writer touched an UNRELATED field -> rebased, both survive
    std::uint64_t dropped = 0;  // co-writer touched THIS field -> loud drop, co-writer's value survives
    bool no_lost_update = true; // the invariant: the world always matches the acknowledged state
    std::uint64_t rounds = 0;
};

// Two clients race on a shared file for `rounds` rounds under a fixed seed. Client A commits a gesture
// through the L-30 engine; client B is a concurrent writer that fires mid-attempt. Each round is one of
// three contention shapes (B collides on A's field / B edits a disjoint field / B is absent). The
// invariant checked EVERY round: the on-disk world equals the acknowledged authoritative state — an
// applied/rebased write is present, and a dropped write NEVER overwrote the co-writer's value.
PropStats run_coedit_property(std::uint64_t seed, std::uint64_t rounds)
{
    SeededRng rng(seed);
    const std::vector<std::string> ptrs = {"/hp", "/mana", "/pos/x", "/pos/y", "/name"};

    CoeditGateway gw;
    std::map<std::string, JsonValue> expected;
    for (std::size_t i = 0; i < ptrs.size(); ++i)
    {
        gw.fields[ptrs[i]] = jint(static_cast<std::int64_t>(i));
        expected[ptrs[i]] = gw.fields[ptrs[i]];
    }

    PropStats st;
    st.rounds = rounds;
    const std::string scene = "root.scene.json";
    const std::vector<std::string> id_path = {"aaaaaaaaaaaaaaa1"};

    for (std::uint64_t round = 0; round < rounds; ++round)
    {
        const std::size_t idx_a = static_cast<std::size_t>(rng.bounded(ptrs.size()));
        const std::string& p_a = ptrs[idx_a];
        const std::uint64_t mode = rng.bounded(3); // 0 = collide, 1 = disjoint, 2 = no co-writer

        // Strictly increasing, mutually-distinct values so A's value, B's value, and the current base
        // are always distinguishable — the L-30 collision compare then resolves deterministically.
        const JsonValue v_a = jint(static_cast<std::int64_t>(round) * 4 + 1);
        const JsonValue v_b = jint(static_cast<std::int64_t>(round) * 4 + 2);

        const std::uint64_t base_hash = gw.file_hash;
        const JsonValue base_val = gw.fields[p_a]; // A's snapshot of its field (the collision base)

        std::string p_b;
        if (mode == 0)
        {
            p_b = p_a; // B collides on A's field
        }
        else if (mode == 1)
        {
            const std::size_t idx_b =
                (idx_a + 1 + static_cast<std::size_t>(rng.bounded(ptrs.size() - 1))) % ptrs.size();
            p_b = ptrs[idx_b]; // guaranteed != p_a
        }

        gw.fired = false;
        if (mode == 2)
            gw.on_first_attempt = nullptr;
        else
            gw.on_first_attempt = [&]() {
                gw.file_hash += 1;
                gw.fields[p_b] = v_b;
            };

        const inspector::CommitResult r = inspector::commit_override_write(
            gw, make_request(scene, id_path, p_a, v_a), scene, id_path, p_a, base_val, base_hash);

        if (mode == 2)
        {
            if (r.status != inspector::CommitResult::Status::applied)
                st.no_lost_update = false;
            expected[p_a] = v_a;
            ++st.applied;
        }
        else if (mode == 1)
        {
            if (r.status != inspector::CommitResult::Status::rebased)
                st.no_lost_update = false;
            expected[p_b] = v_b; // B's write survives...
            expected[p_a] = v_a; // ...and A's rebased write also lands: no lost update
            ++st.rebased;
        }
        else
        {
            if (r.status != inspector::CommitResult::Status::dropped)
                st.no_lost_update = false;
            if (r.code != "cas.mismatch")
                st.no_lost_update = false;
            expected[p_a] = v_b; // B's committed write survives; A was loudly refused, never silent
            ++st.dropped;
        }

        // The load-bearing global invariant: the world matches the acknowledged authoritative state
        // for EVERY field, every round. A single silent overwrite anywhere trips this.
        for (const auto& kv : expected)
            if (!json_eq(gw.fields[kv.first], kv.second))
                st.no_lost_update = false;
    }
    return st;
}

} // namespace

int main()
{
    using Status = inspector::CommitResult::Status;
    const std::string scene = "root.scene.json";
    const std::vector<std::string> id_path = {"aaaaaaaaaaaaaaa1"};

    // --- L-30 rebase-or-drop under a concurrent SECOND client (the explicit edge cases) -------------
    // Clean apply: no co-writer, the CAS-guarded gesture lands.
    {
        CoeditGateway gw;
        gw.fields["/hp"] = jint(1);
        const inspector::CommitResult r = inspector::commit_override_write(
            gw, make_request(scene, id_path, "/hp", jint(2)), scene, id_path, "/hp",
            /*collision_base=*/jint(1), /*base_raw_hash=*/gw.file_hash);
        CHECK(r.status == Status::applied);
        CHECK(json_eq(gw.fields["/hp"], jint(2)));
    }
    // REBASE: a co-writer advanced the file editing an UNRELATED field -> rebase onto the new state;
    // BOTH writes survive (no lost update).
    {
        CoeditGateway gw;
        gw.fields["/hp"] = jint(1);
        gw.fields["/name"] = jstr("a");
        const std::uint64_t base_hash = gw.file_hash;
        gw.on_first_attempt = [&]() {
            gw.file_hash += 1;
            gw.fields["/name"] = jstr("b");
        };
        const inspector::CommitResult r = inspector::commit_override_write(
            gw, make_request(scene, id_path, "/hp", jint(2)), scene, id_path, "/hp",
            /*collision_base=*/jint(1), base_hash);
        CHECK(r.status == Status::rebased);
        CHECK(json_eq(gw.fields["/hp"], jint(2)));    // A's write landed
        CHECK(json_eq(gw.fields["/name"], jstr("b"))); // B's write survived
    }
    // DROP LOUDLY: a co-writer changed THIS field -> the gesture drops with cas.mismatch, NEVER a
    // silent overwrite; the co-writer's value survives.
    {
        CoeditGateway gw;
        gw.fields["/hp"] = jint(1);
        const std::uint64_t base_hash = gw.file_hash;
        gw.on_first_attempt = [&]() {
            gw.file_hash += 1;
            gw.fields["/hp"] = jint(99);
        };
        const inspector::CommitResult r = inspector::commit_override_write(
            gw, make_request(scene, id_path, "/hp", jint(2)), scene, id_path, "/hp",
            /*collision_base=*/jint(1), base_hash);
        CHECK(r.status == Status::dropped);
        CHECK(r.code == "cas.mismatch");           // the honest lost-update signal (no bespoke event)
        CHECK(!r.message.empty());                 // the loud L-30 diagnostic
        CHECK(json_eq(gw.fields["/hp"], jint(99))); // B survived; A did NOT overwrite -> no lost update
    }

    // --- CAS `--if-match`: the raw-byte content hash the write path guards on -----------------------
    {
        const std::string bytes_v1 = R"({"hp":10})";
        const std::string bytes_v2 = R"({"hp":11})";
        const std::uint64_t token_v1 = content_hash(bytes_v1);
        CHECK(content_hash(bytes_v1) == token_v1); // stable: identical bytes -> identical token
        CHECK(content_hash(bytes_v2) != token_v1); // a one-byte edit flips the CAS token
        // An `--if-match <token_v1>` guard against the now-current v2 bytes would MISMATCH (the write
        // is refused with cas.mismatch), while a matching token would apply — the CAS primitive L-50
        // rests on.
        const std::uint64_t current = content_hash(bytes_v2);
        CHECK(token_v1 != current);              // stale --if-match -> refused
        CHECK(content_hash(bytes_v2) == current); // fresh --if-match -> accepted
    }

    // --- the 10,000-round no-lost-update property (the DoD invariant) -------------------------------
    {
        const PropStats st = run_coedit_property(/*seed=*/0xC0FFEEu, /*rounds=*/10000);
        CHECK(st.no_lost_update);                                   // NO lost update in 10k rounds
        CHECK(st.applied + st.rebased + st.dropped == st.rounds);
        CHECK(st.applied > 0); // all three contention shapes were exercised (the property isn't vacuous)
        CHECK(st.rebased > 0);
        CHECK(st.dropped > 0);
    }

    // --- R-QA-010 composition check: the co-edit safety net's substrate converges -------------------
    // The exhaustive 512-seed per-mechanism sweeps live in the dedicated testing- tests; here we
    // confirm the seams the co-edit story depends on converge composed, over a small window that still
    // exercises every watcher-fault class.
    {
        bool saw_drop = false, saw_dup = false, saw_reorder = false;
        for (std::uint64_t seed = 0; seed < 64; ++seed)
        {
            const context::testing::WatcherScenarioResult w =
                context::testing::run_watcher_fault_scenario(seed);
            CHECK(w.converged); // reconcile index == true on-disk content: a co-editor's next CAS
                                // token is the REAL hash, so no lost update is masked by a stale watcher
            saw_drop = saw_drop || w.dropped;
            saw_dup = saw_dup || w.duplicated;
            saw_reorder = saw_reorder || w.reordered;

            const context::testing::CrashScenarioResult c =
                context::testing::run_crash_recovery_scenario(seed);
            CHECK(c.converged()); // multi-file writes serialize atomically through the intent log and
                                  // resume with no torn state (the daemon write-queue guarantee)

            const context::testing::SlowClientScenarioResult s =
                context::testing::run_slow_client_scenario(seed);
            CHECK(s.converged()); // a flooded subscriber gap-marks + re-snapshots, never blocks
        }
        CHECK(saw_drop);
        CHECK(saw_dup);
        CHECK(saw_reorder);
    }

    // --- committed regression seeds (R-QA-011): any minimized failing co-edit seed is pinned here
    //     forever so its exact schedule replays bit-identically on every platform ---------------------
    {
        static const std::uint64_t kRegressionSeeds[] = {0u,       1u,          42u,
                                                         4096u,    0xC0FFEEu,  0xDEADBEEFu};
        for (const std::uint64_t seed : kRegressionSeeds)
        {
            const PropStats st = run_coedit_property(seed, /*rounds=*/2000);
            CHECK(st.no_lost_update);
            CHECK(st.applied + st.rebased + st.dropped == st.rounds);
            CHECK(context::testing::run_watcher_fault_scenario(seed).converged);
            CHECK(context::testing::run_crash_recovery_scenario(seed).converged());
            CHECK(context::testing::run_slow_client_scenario(seed).converged());
        }
    }

    CONC_TEST_MAIN_END();
}
