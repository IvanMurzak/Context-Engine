// M1 exit criterion 3 — kill -9 crash recovery (R-FILE-004 / R-BRIDGE-008, issue #36):
// kill -9 the REAL daemon process MID-multi-file-write and lose nothing — the fsync'd intent log
// resumes the interrupted batch on the next incarnation (entries HMAC-integrity-checked, re-jailed,
// re-CAS'd — never clobbering a file that moved on), and a reconnecting client gets a FRESH
// snapshot on the NEW incarnation (`incarnationId`).
//
// This is the REAL-process leg the issue mandates: a real `context daemon` child, a real SIGKILL /
// TerminateProcess delivered while the daemon is inside the multi-file write (deterministically
// caught: the intent entry is fsync'd BEFORE the first write and cleared AFTER the last durable
// rename, so "entry on disk after the kill" == "killed mid-op"; a lost race is detected and the
// scenario retries with a fresh project). The deterministic crash-point SWEEP between every durable
// step lives in the R-QA-010 harness suites (src/testing/test_crash_recovery.cpp over the seams;
// src/editor/filesync/tests/test_native_crash_recovery.cpp on the real disk) — this test adds the
// end-to-end proof over the real process + real wire (#35).
//
// R-QA-013 coverage: happy path (interrupted batch fully resumed — nothing lost), failure path
// (a file that MOVED ON after the crash is NOT clobbered: re-CAS diagnostic naming the op), and the
// reconnect edge (fresh snapshot, new incarnationId, recovery diagnostics visible to the client;
// a subsequent clean batch completes normally).

#include "context/editor/contract/json.h"
#include "context/editor/derivation/canonical_parse.h"

#include "integration_test.h"
#include "process_util.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using itest::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace
{

constexpr int kBatchFiles = 150;

// Ceiling for the post-recovery daemon process-EXIT waits (the killed daemon reaping mid-op, and
// daemon2 exiting after a clean `shutdown`). A killed process reaps at once and a clean shutdown
// returns in well under a second, but on a saturated CI runner (many parallel jobs contending) the
// daemon2 exit has overshot a tight 15s ceiling on a NON-sanitizer leg — the failing
// `CHECK failed: daemon2_code == 0` at line ~325 on `shader-crosscompile (macos-latest)` (owner-
// flagged run 29201369515: attempt 1 RED, attempt 2 GREEN, same commit). CE PR #202 widened only the
// two `wait_for_instance` BOOT waits and only under sanitizers, so these UNSCALED process-exit waits
// stayed load-fragile. 60s is load-safe headroom (~4x the observed overshoot) yet far below the 600s
// ctest TIMEOUT, and it is routed through itest::scaled_timeout_ms() so the sanitizer legs keep their
// instrumentation headroom too. The REAL kill-9-then-recover assertion is unchanged: a crashed daemon
// still returns a non-zero code immediately, and a genuinely non-recovering (hung) daemon still trips
// this ceiling and fails `CHECK(daemon..._done)` — only the false-timeout-under-load is removed.
constexpr int kDaemonExitWaitMs = 60000;

std::string batch_path(int i)
{
    return "proj/f" + std::to_string(i) + ".scene";
}

// Distinct, non-trivial content per file: each durable temp+fsync+rename does real work, widening
// the kill window between the intent-entry fsync and the batch's final clear.
std::string batch_content(int i)
{
    std::string filler(1800, 'x');
    return "entity: " + std::to_string(i) + "\n" + filler + "-" + std::to_string(i) + "\n";
}

Json build_batch_params()
{
    Json files = Json::array();
    for (int i = 0; i < kBatchFiles; ++i)
    {
        Json f = Json::object();
        f.set("path", Json(batch_path(i)));
        f.set("content", Json(batch_content(i)));
        files.push_back(std::move(f));
    }
    Json params = Json::object();
    params.set("files", std::move(files));
    return params;
}

// A COMMITTED intent entry (not an in-flight atomic-write temp — those carry the ".tmp" /
// ".tmp.<unique>" staging shapes). Only a committed entry means the op durably BEGAN
// (fsync-before-first-write); killing before the entry's rename would be killing before the op
// started, and recovery would rightly find nothing to resume.
bool intent_dir_has_committed_entry(const fs::path& project)
{
    const fs::path dir = project / "proj" / ".editor" / "intent";
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return false;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        const std::string name = entry.path().filename().string();
        if (name.size() >= 4 && name.substr(name.size() - 4) == ".tmp")
            continue;
        if (name.find(".tmp.") != std::string::npos)
            continue;
        return true;
    }
    return false;
}

// Mid-op is observable when the op durably began (committed intent entry) AND the batch's first
// write already landed — from that point until the final clear the daemon is guaranteed inside
// the multi-file apply, with ~kBatchFiles-1 durable renames still ahead (a wide kill window).
bool mid_op_observable(const fs::path& project)
{
    std::error_code ec;
    return intent_dir_has_committed_entry(project) &&
           fs::exists(project / "proj" / "f0.scene", ec);
}

struct MidOpKill
{
    fs::path project;
    std::string incarnation_before;
};

// One attempt: boot a daemon, launch a batch, kill -9 the daemon the moment the intent entry
// appears. Returns the project + pre-kill incarnation on a confirmed MID-OP kill; nullopt when the
// batch won the race (completed + cleared before the kill landed) — the caller retries fresh.
std::optional<MidOpKill> attempt_mid_op_kill(const std::string& bin, int attempt)
{
    const fs::path project =
        itest::make_temp_project(("kill9-a" + std::to_string(attempt)).c_str());

    ctest_proc::Process daemon = ctest_proc::spawn(bin, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));
    // Widen the daemon-boot discovery-hint wait under a sanitizer (instrumented boot is far
    // slower under concurrent runner load — see integration_test.h § sanitizer-aware timeout).
    CHECK(itest::wait_for_instance(project, itest::scaled_timeout_ms(15000)));

    itest::RpcClient rpc;
    CHECK(rpc.connect(project));
    const auto attached = rpc.attach(1, {"describe"}, "write");
    CHECK(itest::is_ok(attached));

    std::string incarnation_before;
    const auto snap = rpc.call("snapshot", Json::object());
    CHECK(itest::is_ok(snap));
    if (itest::is_ok(snap))
        incarnation_before = snap->at("result").at("data").at("incarnationId").as_string();
    CHECK(!incarnation_before.empty());

    // Fire the multi-file write from a helper thread (the call blocks for a reply that will never
    // come once the daemon is killed mid-op — the dead transport returns nullopt and the thread
    // exits).
    std::atomic<bool> batch_returned{false};
    std::thread batcher(
        [&rpc, &batch_returned]()
        {
            (void)rpc.call("edit-batch", build_batch_params());
            batch_returned.store(true);
        });

    // The moment the fsync'd intent entry appears the daemon is INSIDE the multi-file op
    // (fsync-before-first-write). Kill it right there — the real kill -9. Not seeing the entry
    // before the batch reply is a LOST RACE (the op completed + cleared first), not a failure:
    // the caller retries on a fresh project; the outer CHECK gates that one attempt succeeded.
    bool saw_mid_op = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (mid_op_observable(project))
        {
            saw_mid_op = true;
            break;
        }
        if (batch_returned.load())
            break; // batch finished before we ever observed it mid-op — race lost
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ctest_proc::kill(daemon); // SIGKILL / TerminateProcess — no cleanup runs in the daemon
    int daemon_code = -1;
    // Load-safe, sanitizer-aware ceiling for the killed daemon's reap (see kDaemonExitWaitMs).
    (void)ctest_proc::wait_for(daemon, itest::scaled_timeout_ms(kDaemonExitWaitMs), daemon_code);
    ctest_proc::release(daemon);

    batcher.join();
    rpc.close();

    // Confirmed mid-op iff the committed entry is STILL pending after the kill (a cleared entry
    // means the op completed before the kill landed — retry the scenario).
    if (!saw_mid_op || !intent_dir_has_committed_entry(project))
    {
        std::error_code ec;
        fs::remove_all(project, ec);
        return std::nullopt;
    }
    return MidOpKill{project, incarnation_before};
}

} // namespace

int main()
{
    const std::string bin = CONTEXT_BINARY;

    // ---- kill -9 the daemon mid-multi-file-write (bounded retries for the inherent race) --------
    std::optional<MidOpKill> killed;
    for (int attempt = 0; attempt < 5 && !killed.has_value(); ++attempt)
        killed = attempt_mid_op_kill(bin, attempt);
    CHECK(killed.has_value());
    if (!killed.has_value())
        ITEST_MAIN_END(); // no point continuing without a mid-op crash

    const fs::path project = killed->project;
    const std::string incarnation_before = killed->incarnation_before;

    // ---- the moved-on file (failure path): mutate ONE batch target after the crash --------------
    // Whatever state the kill left f2 in (absent, or already holding its target), this write moves
    // it PAST the planning-time CAS hash — recovery must refuse to clobber it (L-25: no rollback,
    // no forced state) and diagnose instead.
    const std::string moved_on = "moved on after the crash - do not clobber";
    CHECK(itest::write_file_raw(project / "proj" / "f2.scene", moved_on));

    // ---- restart: a NEW daemon incarnation on the same project ----------------------------------
    // The killed process's single-instance lock died with it (R-BRIDGE-001 — the OS releases the
    // lock), so the restart BOOTS; start() runs the R-FILE-004 recovery pass before serving.
    const std::string stale_hint = itest::read_file(project / ".editor" / "instance.json");
    ctest_proc::Process daemon2 = ctest_proc::spawn(bin, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon2));
    // Same sanitizer-aware widening for the post-crash restart boot (see the first wait above).
    CHECK(itest::wait_for_instance(project, itest::scaled_timeout_ms(15000), stale_hint));

    // ---- a RECONNECTING (read-only) client gets a fresh snapshot on the NEW incarnation ---------
    {
        itest::RpcClient rpc;
        CHECK(rpc.connect(project));
        const auto attached = rpc.attach(1, {"describe"}, ""); // read/query baseline only
        CHECK(itest::is_ok(attached));

        const auto snap = rpc.call("snapshot", Json::object());
        CHECK(itest::is_ok(snap));
        if (itest::is_ok(snap))
        {
            const Json& data = snap->at("result").at("data");
            // R-BRIDGE-008: a restart is a NEW incarnation epoch — a stale "since seq N" cursor is
            // invalid; the fresh snapshot is the client's re-entry point.
            CHECK(data.at("incarnationId").as_string() != incarnation_before);
            CHECK(!data.at("incarnationId").as_string().empty());

            // The recovery pass surfaced the moved-on file as a machine-readable CAS diagnostic
            // NAMING the incomplete op (R-FILE-003) — visible to any reconnecting client.
            bool saw_cas = false;
            const Json& diags = data.at("recoveryDiagnostics");
            for (std::size_t i = 0; i < diags.size(); ++i)
            {
                const Json& d = diags.at(i);
                if (d.at("code").as_string() != "filesync.intent.cas")
                    continue;
                saw_cas = true;
                // The op id names the CRASHED incarnation's batch (batch-<incarnation>-<n>).
                CHECK(d.at("opId").as_string().find(incarnation_before) != std::string::npos);
                bool names_f2 = false;
                for (std::size_t j = 0; j < d.at("remainingWrites").size(); ++j)
                    if (d.at("remainingWrites").at(j).as_string() == batch_path(2))
                        names_f2 = true;
                CHECK(names_f2);
            }
            CHECK(saw_cas);
        }

        // The recovered writes reach the derived world: reconcile + query one resumed file over
        // the wire and match its canonical hash against the batch's intended content.
        const auto reconciled = rpc.call("reconcile", Json::object());
        CHECK(itest::is_ok(reconciled));
        Json q = Json::object();
        q.set("path", Json(batch_path(7)));
        const auto node = rpc.call("query", std::move(q));
        CHECK(itest::is_ok(node));
        if (itest::is_ok(node))
        {
            const Json& data = node->at("result").at("data");
            CHECK(data.at("present").as_bool());
            CHECK(data.at("canonicalHash").as_string() ==
                  std::to_string(context::editor::derivation::canonical_parse(batch_content(7))
                                     .canonical_hash));
        }
        rpc.close();
    }

    // ---- nothing lost on disk: the interrupted batch was resumed to completion ------------------
    for (int i = 0; i < kBatchFiles; ++i)
    {
        const std::string on_disk = itest::read_file(project / "proj" / ("f" + std::to_string(i) +
                                                                         ".scene"));
        if (i == 2)
            CHECK(on_disk == moved_on); // re-CAS'd: the moved-on file was NOT clobbered
        else
            CHECK(on_disk == batch_content(i)); // resumed: every other write durably landed
    }
    // The not-fully-resumable op stays on disk (honest post-crash reporting, never silent discard).
    CHECK(intent_dir_has_committed_entry(project));

    // ---- the new incarnation serves writes normally: a clean batch completes + clears -----------
    {
        itest::RpcClient rpc;
        CHECK(rpc.connect(project));
        const auto attached = rpc.attach(1, {"describe"}, "write,session");
        CHECK(itest::is_ok(attached));

        Json files = Json::array();
        for (int i = 0; i < 3; ++i)
        {
            Json f = Json::object();
            f.set("path", Json("proj/clean" + std::to_string(i) + ".scene"));
            f.set("content", Json("entity: clean-" + std::to_string(i)));
            files.push_back(std::move(f));
        }
        Json params = Json::object();
        params.set("files", std::move(files));
        const auto batch = rpc.call("edit-batch", std::move(params));
        CHECK(itest::is_ok(batch));
        if (itest::is_ok(batch))
        {
            const Json& data = batch->at("result").at("data");
            CHECK(data.at("allReflected").as_bool());
            CHECK(data.at("files").size() == 3);
        }
        for (int i = 0; i < 3; ++i)
            CHECK(itest::read_file(project / "proj" /
                                   ("clean" + std::to_string(i) + ".scene")) ==
                  "entity: clean-" + std::to_string(i));

        const auto stopped = rpc.call("shutdown", Json::object());
        CHECK(itest::is_ok(stopped));
        rpc.close();
    }

    int daemon2_code = -1;
    // Load-safe, sanitizer-aware ceiling for daemon2's post-recovery clean exit (see kDaemonExitWaitMs)
    // — this is the wait that false-timed-out under concurrent runner load on the macos leg.
    const bool daemon2_done =
        ctest_proc::wait_for(daemon2, itest::scaled_timeout_ms(kDaemonExitWaitMs), daemon2_code);
    if (!daemon2_done)
        ctest_proc::kill(daemon2);
    ctest_proc::release(daemon2);
    CHECK(daemon2_done);
    if (daemon2_done)
        CHECK(daemon2_code == 0);

    std::error_code ec;
    fs::remove_all(project, ec);

    ITEST_MAIN_END();
}
