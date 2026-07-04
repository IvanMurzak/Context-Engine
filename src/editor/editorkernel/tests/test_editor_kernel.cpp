// Composed-loop integration smoke (M1 DoD): the merged file-authoritative libraries, wired into ONE
// runnable headless attach path, prove the end-to-end loop:
//   * the daemon boots (single-instance lock acquired) and a client attaches over the handshake;
//   * a CLI-verb edit (through filesync atomic-IO) reaches the derived World, honoring the
//     read-your-writes barrier (R-CLI-006);
//   * a RAW out-of-band edit of the same file also reaches the derived World (content hash is
//     authoritative, R-FILE-002);
//   * a second instance on the same Project gets the attach signal (R-BRIDGE-001);
//   * a scope-denied edit returns the R-SEC-007 permission class (exit code 6).
// This is the SMOKE scope of issue #30; the full 5-criteria M1 gate (kill-9 recovery, CLI≡RPC≡MCP
// parity, scope-denial e2e) is the follow-up exit-integration task.

#include "context/editor/editorkernel/editor_kernel.h"

#include "context/editor/bridge/event_stream.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/platform.h"

#include "editorkernel_test.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
using context::editor::contract::ClientHandshake;
using context::editor::contract::Envelope;
using context::editor::derivation::canonical_parse;
using context::editor::editorkernel::BatchEdit;
using context::editor::editorkernel::EditBatchOutcome;
using context::editor::editorkernel::EditorKernel;
using context::editor::editorkernel::EditorKernelConfig;
using context::editor::editorkernel::EditOutcome;
using context::editor::filesync::MemoryFileStore;
using context::editor::filesync::NullWatcher;
using context::editor::bridge::Scope;
using context::editor::bridge::ScopeSet;
using context::editor::bridge::Session;
using context::editor::bridge::StartOutcome;
using context::editor::bridge::Subscriber;

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
                   ("ctx-editorkernel-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

EditorKernelConfig config_for(const fs::path& project)
{
    EditorKernelConfig cfg;
    cfg.project_root = project;                     // real FS: the daemon single-instance lock
    cfg.filesync_root = "proj";                     // logical FileStore root (path jail)
    cfg.index_path = "proj/.editor/reconcile-index"; // under the injectable FileStore seam
    return cfg;
}
} // namespace

int main()
{
    const fs::path project = make_temp_project("loop");

    MemoryFileStore store;
    NullWatcher watcher; // portable default: always degraded -> correctness comes from the crawl
    context::kernel::ManualClock clock;
    context::kernel::InlineTaskRunner tasks;

    EditorKernel kernel(store, watcher, clock, tasks, config_for(project));

    // --- boot: the daemon owns the Project (lock acquired), stream + dispatcher up ----------------
    CHECK(kernel.start(ScopeSet::all()) == StartOutcome::booted);
    CHECK(kernel.running());
    CHECK(kernel.daemon().lock().held());
    CHECK(kernel.generation() == 0);

    // --- a second instance on the SAME project gets the attach signal (single-instance, R-BRIDGE-001)
    {
        MemoryFileStore store2;
        NullWatcher watcher2;
        context::kernel::ManualClock clock2;
        context::kernel::InlineTaskRunner tasks2;
        EditorKernel second(store2, watcher2, clock2, tasks2, config_for(project));
        CHECK(second.start() == StartOutcome::attach);
        CHECK(!second.running());
    }

    // --- a client attaches over the capability handshake, scopes clamped to the launch ceiling ----
    {
        ClientHandshake client;
        client.protocol_major = 0;
        client.capabilities = {"describe"};
        auto result = kernel.attach(client, ScopeSet::parse("write"));
        CHECK(std::holds_alternative<Session>(result));
        const Session& session = std::get<Session>(result);
        CHECK(session.attached);
        CHECK(session.scopes.has(Scope::file_write));
        CHECK(!session.scopes.has(Scope::build_install)); // not requested
    }

    // --- CLI-verb edit through filesync atomic-IO reaches the derived World (read-your-writes) -----
    const ScopeSet writer = ScopeSet::parse("write");
    {
        EditOutcome edit = kernel.edit_file("proj/a.scene", "entity: 1", writer);
        CHECK(edit.ok);
        CHECK(edit.error_code.empty());
        CHECK(edit.ticket.path == "proj/a.scene");
        CHECK(edit.ticket.canonical_hash == canonical_parse("entity: 1").canonical_hash);
        // The bytes actually landed on disk through atomic-IO.
        CHECK(store.exists("proj/a.scene"));
        CHECK(*store.read("proj/a.scene") == "entity: 1");

        // Before a pass runs the derived world has NOT changed yet (the write is pending).
        CHECK(kernel.generation() == 0);

        // Read-your-writes barrier (R-CLI-006): bounded-block until the world reflects the write's
        // canonical hash, then the query observes it.
        auto observed = kernel.query_after_hash("proj/a.scene", edit.ticket.canonical_hash);
        CHECK(observed.has_value());
        CHECK(observed->canonical_hash == edit.ticket.canonical_hash);
        CHECK(kernel.generation() >= 1);
        CHECK(kernel.world().alive_count() == 1);

        // The envelope a CLI/RPC caller returns: ok, generationAfter set, exit 0.
        const Envelope env = edit.envelope();
        CHECK(env.ok());
        CHECK(env.exit_code() == 0);
    }

    // --- a RAW out-of-band edit of the SAME file also reaches the derived World --------------------
    {
        const std::uint64_t before = kernel.query("proj/a.scene")->canonical_hash;

        // Bypass the kernel: mutate the file directly on the FileStore (a hand edit / another tool).
        clock.advance(1'000'000'000ULL); // past the self-echo TTL — belt-and-braces (hash differs anyway)
        CHECK(store.write("proj/a.scene", "entity: 2"));

        auto changes = kernel.ingest_external();
        bool saw = false;
        for (const auto& c : changes)
            if (c.path == "proj/a.scene")
                saw = true;
        CHECK(saw); // the crawl detected the raw edit (content hash authoritative, R-FILE-002)

        const std::uint64_t expected = canonical_parse("entity: 2").canonical_hash;
        kernel.settle();
        auto observed = kernel.query("proj/a.scene");
        CHECK(observed.has_value());
        CHECK(observed->canonical_hash == expected);
        CHECK(observed->canonical_hash != before); // the derived value genuinely changed
        CHECK(kernel.world().alive_count() == 1);   // same source entity, new derived value
    }

    // --- R-SEC-007: a scope-denied edit is refused with the permission class (exit code 6) ---------
    {
        const std::uint64_t gen_before = kernel.generation();
        EditOutcome denied = kernel.edit_file("proj/b.scene", "entity: 9", ScopeSet::read_query());
        CHECK(!denied.ok);
        CHECK(denied.error_code == "scope.denied");

        const Envelope env = denied.envelope();
        CHECK(!env.ok());
        CHECK(env.exit_code() == 6); // permission class (was generic 1 before the catalog fix)

        // Nothing was written and the world did not change.
        CHECK(!store.exists("proj/b.scene"));
        CHECK(kernel.generation() == gen_before);
    }

    // --- a path-jail escape is refused (R-SEC-008), even with the write scope ---------------------
    {
        EditOutcome escape = kernel.edit_file("proj/../escape.scene", "nope", writer);
        CHECK(!escape.ok);
        CHECK(escape.error_code == "path.jail_violation");
        CHECK(escape.envelope().exit_code() == 6);
        CHECK(!store.exists("escape.scene"));
    }

    // --- await_hash / await_generation bounded-block THEN publish the settle fact (R-CLI-006 +
    // --- R-BRIDGE-008): the explicit barrier primitives, distinct from query_after_hash (which does
    // --- not settle). Subscribe to the client `derivation` stream to observe the quiescence event. --
    {
        Subscriber sub({"derivation"});
        kernel.events().add_subscriber(&sub);

        // A fresh CLI-verb edit leaves a pending derivation; await_hash blocks until the derived world
        // reflects the write's canonical hash, then settles.
        EditOutcome edit = kernel.edit_file("proj/c.scene", "entity: 3", writer);
        CHECK(edit.ok);
        const auto hashed = kernel.await_hash(edit.ticket.canonical_hash);
        CHECK(hashed.ok());
        auto node = kernel.query("proj/c.scene");
        CHECK(node.has_value());
        CHECK(node->canonical_hash == edit.ticket.canonical_hash);

        // await_hash's settle() published derivation.settled carrying the derived-world generation.
        const std::uint64_t settled_gen = kernel.generation();
        bool saw_settled = false;
        for (const auto& e : sub.drain())
            if (e.topic == "derivation" &&
                e.payload.at("event").as_string() == "derivation.settled" &&
                static_cast<std::uint64_t>(e.payload.at("generation").as_int()) == settled_gen)
                saw_settled = true;
        CHECK(saw_settled);

        // A foreign-generation barrier on an already-reached generation resolves immediately (0 passes)
        // and re-settles. wait_for_generation is otherwise unexercised.
        const auto reached = kernel.await_generation(kernel.generation());
        CHECK(reached.ok());
        CHECK(reached.passes == 0);

        kernel.events().remove_subscriber(&sub);
    }

    kernel.stop();
    CHECK(!kernel.running());

    // --- a read-your-writes query must NEVER stall behind a derivation backlog (R-FILE-013): under
    // --- load-shed, query_after_hash marks the queried path visible so it derives first. Without that
    // --- the target — sorted last behind a backlog — is deferred past the barrier bound and times out.
    {
        const fs::path loadshed_project = make_temp_project("loadshed");
        MemoryFileStore ls_store;
        NullWatcher ls_watcher;
        context::kernel::ManualClock ls_clock;
        context::kernel::InlineTaskRunner ls_tasks;

        EditorKernelConfig ls_cfg = config_for(loadshed_project);
        ls_cfg.derivation.high_watermark = 2;     // overload once >2 writes are pending
        ls_cfg.derivation.max_batch_per_pass = 1; // …and derive only ONE non-visible node per pass
        ls_cfg.barrier_max_passes = 3;            // a bound too small to reach a late-sorted backlog entry

        EditorKernel ls_kernel(ls_store, ls_watcher, ls_clock, ls_tasks, ls_cfg);
        CHECK(ls_kernel.start(ScopeSet::all()) == StartOutcome::booted);

        // Enqueue a backlog of distinct writes without running a pass; the target sorts LAST.
        EditOutcome target;
        for (int i = 0; i < 8; ++i)
        {
            const std::string path = "proj/f" + std::to_string(i) + ".scene";
            EditOutcome e = ls_kernel.edit_file(path, "entity: " + std::to_string(100 + i), writer);
            CHECK(e.ok);
            if (i == 7)
                target = e; // proj/f7.scene sorts after proj/f0..f6 in the pending map
        }
        CHECK(ls_kernel.generation() == 0); // nothing derived yet — the whole backlog is pending

        // Resolves within the tight bound ONLY because the query prioritizes its own path.
        auto observed = ls_kernel.query_after_hash("proj/f7.scene", target.ticket.canonical_hash);
        CHECK(observed.has_value());
        CHECK(observed->canonical_hash == target.ticket.canonical_hash);

        ls_kernel.stop();
        std::error_code ls_ec;
        fs::remove_all(loadshed_project, ls_ec);
    }

    // --- read-your-writes is per-PATH, not per-hash (M1 exit-gate regression): when ANOTHER file
    // --- already carries the identical canonical content, the world-global hash barrier resolves
    // --- instantly — query_after_hash must still drain the queried path's own pending ingest, so
    // --- the second write's read never observes the pre-write state of ITS OWN node.
    {
        const fs::path dup_project = make_temp_project("dup-content");
        MemoryFileStore dup_store;
        NullWatcher dup_watcher;
        context::kernel::ManualClock dup_clock;
        context::kernel::InlineTaskRunner dup_tasks;

        EditorKernel dup_kernel(dup_store, dup_watcher, dup_clock, dup_tasks,
                                config_for(dup_project));
        CHECK(dup_kernel.start(ScopeSet::all()) == StartOutcome::booted);

        // File 1 derives content C; its canonical hash is now reflected by a live node.
        const EditOutcome first = dup_kernel.edit_file("proj/one.scene", "entity: same", writer);
        CHECK(first.ok);
        auto first_node = dup_kernel.query_after_hash("proj/one.scene", first.ticket.canonical_hash);
        CHECK(first_node.has_value());

        // File 2 writes the IDENTICAL content: the hash barrier alone would resolve on file 1's
        // node before file 2's ingest ever derived. The per-path drain must still produce file 2.
        const EditOutcome second = dup_kernel.edit_file("proj/two.scene", "entity: same", writer);
        CHECK(second.ok);
        CHECK(second.ticket.canonical_hash == first.ticket.canonical_hash); // same canonical form
        auto second_node =
            dup_kernel.query_after_hash("proj/two.scene", second.ticket.canonical_hash);
        CHECK(second_node.has_value());
        CHECK(second_node.has_value() &&
              second_node->canonical_hash == second.ticket.canonical_hash);
        CHECK(dup_kernel.world().alive_count() == 2); // both files live in the derived World

        dup_kernel.stop();
        std::error_code dup_ec;
        fs::remove_all(dup_project, dup_ec);
    }

    // --- edit_files refuses a batch naming the same path twice (R-FILE-004 resume correctness):
    // --- every intent entry's planning-time CAS hash is measured against the PRE-batch file, so a
    // --- twice-applied path would make a mid-batch crash recovery re-CAS the second entry against
    // --- the FIRST apply's bytes — misreporting a moved-on file instead of resuming the batch.
    {
        const fs::path dupbatch_project = make_temp_project("dup-batch");
        MemoryFileStore db_store;
        NullWatcher db_watcher;
        context::kernel::ManualClock db_clock;
        context::kernel::InlineTaskRunner db_tasks;

        EditorKernel db_kernel(db_store, db_watcher, db_clock, db_tasks,
                               config_for(dupbatch_project));
        CHECK(db_kernel.start(ScopeSet::all()) == StartOutcome::booted);

        const std::vector<BatchEdit> dup_batch = {
            {"proj/x.scene", "entity: first"},
            {"proj/y.scene", "entity: other"},
            {"proj/x.scene", "entity: second"}, // the same path again — refused at planning
        };
        const EditBatchOutcome refused = db_kernel.edit_files(dup_batch, writer);
        CHECK(!refused.ok);
        CHECK(refused.error_code == "usage.invalid");
        CHECK(refused.error_detail.find("proj/x.scene") != std::string::npos);
        // Planning-stage refusal: NOTHING was written — no partial batch, no intent entry.
        CHECK(!db_store.exists("proj/x.scene"));
        CHECK(!db_store.exists("proj/y.scene"));

        // The same content batch WITHOUT the duplicate goes through (the guard is not over-broad).
        const std::vector<BatchEdit> clean_batch = {
            {"proj/x.scene", "entity: second"},
            {"proj/y.scene", "entity: other"},
        };
        const EditBatchOutcome accepted = db_kernel.edit_files(clean_batch, writer);
        CHECK(accepted.ok);
        CHECK(*db_store.read("proj/x.scene") == "entity: second");
        CHECK(*db_store.read("proj/y.scene") == "entity: other");

        db_kernel.stop();
        std::error_code db_ec;
        fs::remove_all(dupbatch_project, db_ec);
    }

    // Best-effort cleanup of the real lock directory.
    std::error_code ec;
    fs::remove_all(project, ec);

    EDITORKERNEL_TEST_MAIN_END();
}
