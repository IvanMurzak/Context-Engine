// Watch-hash-reconcile pipeline (R-FILE-002 / R-QA-010):
//   * a watcher hint drives an unconditional re-hash;
//   * content hash is AUTHORITATIVE — duplicated/reordered hints never corrupt state;
//   * a DROPPED watcher event is still reconciled by the crawl (the safety net);
//   * a same-mtime+size in-place edit is missed by the gated crawl but caught by the full re-hash;
//   * the daemon's own write is self-echo-suppressed;
//   * a degraded watcher emits a visible watcher.degraded log event (never a silent fall-back);
//   * a touch (mtime bump, unchanged content) refreshes the index's cheap gate so a later gated
//     crawl short-circuits instead of re-reading + re-hashing the file on every pass.

#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/reconciler.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/event_bus.h"
#include "context/kernel/platform.h"
#include "filesync_test.h"

#include <string>
#include <vector>

using namespace context::editor::filesync;

namespace
{

bool has_change(const std::vector<ReconcileChange>& changes, const std::string& path, ChangeType type)
{
    for (const ReconcileChange& change : changes)
    {
        if (change.path == path && change.type == type)
            return true;
    }
    return false;
}

} // namespace

int main()
{
    // --- 1. hinted create + modify; duplicate/reorder hints are harmless -------------------------
    {
        MemoryFileStore fs;
        FakeWatcher watcher;
        context::kernel::ManualClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(fs, watcher, clock, tasks, "proj", "proj/.editor/index");

        fs.write("proj/a.txt", "A0"); // external creation
        watcher.emit("proj/a.txt", ChangeKind::created);
        auto changes = rec.reconcile_hints();
        CHECK(has_change(changes, "proj/a.txt", ChangeType::created));
        CHECK(changes.size() == 1);
        CHECK(rec.index().get("proj/a.txt")->content_hash == content_hash("A0"));

        fs.write("proj/a.txt", "A1"); // external modify
        watcher.emit("proj/a.txt", ChangeKind::modified);
        changes = rec.reconcile_hints();
        CHECK(has_change(changes, "proj/a.txt", ChangeType::modified));

        // Content is now A1 in the index. Duplicated + reordered hints for an UNCHANGED file yield no
        // change — the hash is the truth, the watcher is only a hint.
        watcher.emit("proj/a.txt", ChangeKind::modified);
        watcher.duplicate_all();
        watcher.reverse();
        changes = rec.reconcile_hints();
        CHECK(changes.empty());
    }

    // --- 2. dropped watcher event -> still reconciled by the (gated) crawl safety net ------------
    {
        MemoryFileStore fs;
        FakeWatcher watcher;
        context::kernel::ManualClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(fs, watcher, clock, tasks, "proj", "proj/.editor/index");

        fs.write("proj/b.txt", "B0");
        watcher.emit("proj/b.txt", ChangeKind::created);
        (void)rec.reconcile_hints();

        // External edit whose size changes, but EVERY watcher event is lost.
        fs.write("proj/b.txt", "B0-and-more");
        watcher.emit("proj/b.txt", ChangeKind::modified);
        watcher.drop_all();
        CHECK(rec.reconcile_hints().empty()); // nothing to reconcile — the hint was lost

        // The crawl converges anyway (size differs -> the mtime+size gate catches it).
        auto crawled = rec.crawl(/*gated=*/true);
        CHECK(has_change(crawled, "proj/b.txt", ChangeType::modified));
        CHECK(rec.index().get("proj/b.txt")->content_hash == content_hash("B0-and-more"));
    }

    // --- 3. same-mtime+size in-place edit: gated crawl MISSES it, full re-hash CATCHES it --------
    {
        MemoryFileStore fs;
        FakeWatcher watcher;
        context::kernel::ManualClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(fs, watcher, clock, tasks, "proj", "proj/.editor/index");

        fs.write("proj/c.txt", "C0");
        watcher.emit("proj/c.txt", ChangeKind::created);
        (void)rec.reconcile_hints();
        const std::uint64_t pinned_mtime = rec.index().get("proj/c.txt")->mtime_nanos;

        // Same-size in-place edit; pin mtime back so mtime+size are byte-for-byte identical (models a
        // same-second edit — mtime granularity is documented as untrusted for content equality).
        fs.write("proj/c.txt", "C1"); // same length as "C0"
        fs.set_mtime("proj/c.txt", pinned_mtime);
        watcher.emit("proj/c.txt", ChangeKind::modified);
        watcher.drop_all(); // and the event is lost

        // Gated crawl: mtime+size match the index -> skipped -> the edit is (correctly) not seen here.
        CHECK(rec.crawl(/*gated=*/true).empty());
        CHECK(rec.index().get("proj/c.txt")->content_hash == content_hash("C0")); // still stale

        // The low-frequency FULL re-hash crawl is the ultimate safety net -> it converges.
        auto full = rec.crawl(/*gated=*/false);
        CHECK(has_change(full, "proj/c.txt", ChangeType::modified));
        CHECK(rec.index().get("proj/c.txt")->content_hash == content_hash("C1"));
    }

    // --- 4. self-echo suppression: the daemon's own write is not re-processed as external ---------
    {
        MemoryFileStore fs;
        FakeWatcher watcher;
        context::kernel::ManualClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(fs, watcher, clock, tasks, "proj", "proj/.editor/index");

        CHECK(rec.apply_write("proj/d.txt", "D0")); // daemon-initiated write
        CHECK(*fs.read("proj/d.txt") == "D0");
        CHECK(rec.index().get("proj/d.txt")->content_hash == content_hash("D0"));

        // The watcher echoes the daemon's OWN write back -> suppressed, no external change.
        watcher.emit("proj/d.txt", ChangeKind::modified);
        CHECK(rec.reconcile_hints().empty());

        // A genuine LATER external edit of the same path is still reconciled (suppression consumed).
        fs.write("proj/d.txt", "D1");
        watcher.emit("proj/d.txt", ChangeKind::modified);
        CHECK(has_change(rec.reconcile_hints(), "proj/d.txt", ChangeType::modified));

        // The daemon's write never escaped the jail.
        CHECK(!rec.apply_write("proj/../escape.txt", "nope"));
        CHECK(!fs.exists("escape.txt"));
    }

    // --- 5. degraded watcher emits a visible watcher.degraded log event (once per transition) -----
    {
        MemoryFileStore fs;
        FakeWatcher watcher;
        context::kernel::ManualClock clock;
        context::kernel::InlineTaskRunner tasks;
        context::kernel::EventBus bus;

        std::vector<std::string> logs;
        bus.subscribe<context::kernel::LogEvent>(
            [&](const context::kernel::LogEvent& event) { logs.push_back(event.message); });

        Reconciler rec(fs, watcher, clock, tasks, "proj", "proj/.editor/index", &bus);
        watcher.set_degraded(true);
        (void)rec.reconcile_hints();
        (void)rec.reconcile_hints(); // still degraded -> not re-announced

        CHECK(logs.size() == 1);
        CHECK(logs[0].find("watcher.degraded") != std::string::npos);
    }

    // --- 6. warm attach reload + deletion detection ----------------------------------------------
    {
        MemoryFileStore fs;
        FakeWatcher watcher;
        context::kernel::ManualClock clock;
        context::kernel::InlineTaskRunner tasks;

        {
            Reconciler rec(fs, watcher, clock, tasks, "proj", "proj/.editor/index");
            CHECK(rec.apply_write("proj/a.txt", "A"));
            CHECK(rec.apply_write("proj/b.txt", "B"));
            CHECK(rec.save_index());
        }

        // A fresh daemon incarnation warm-attaches from the persisted index — no cold re-hash needed.
        Reconciler rec2(fs, watcher, clock, tasks, "proj", "proj/.editor/index");
        CHECK(rec2.attach() == 2);

        // External deletion of b.txt -> the crawl reports the removal and drops the index row.
        CHECK(fs.remove("proj/b.txt"));
        auto changes = rec2.crawl(/*gated=*/true);
        CHECK(has_change(changes, "proj/b.txt", ChangeType::removed));
        CHECK(!rec2.index().get("proj/b.txt").has_value());
    }

    // --- 7. touch (mtime bump, content unchanged) refreshes the index's cheap gate ---------------
    // A bare `touch` bumps mtime without changing bytes. rehash reports no change (the hash is the
    // truth) but MUST refresh the index's stat, or the mtime+size gate is permanently defeated for
    // that file (every gated crawl re-reads + re-hashes it forever — the exact cost the index avoids).
    {
        MemoryFileStore fs;
        FakeWatcher watcher;
        context::kernel::ManualClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(fs, watcher, clock, tasks, "proj", "proj/.editor/index");

        fs.write("proj/e.txt", "E0");
        watcher.emit("proj/e.txt", ChangeKind::created);
        (void)rec.reconcile_hints();
        const std::uint64_t mtime0 = rec.index().get("proj/e.txt")->mtime_nanos;

        // Touch: bump mtime, identical bytes.
        const std::uint64_t bumped = mtime0 + 4242;
        fs.set_mtime("proj/e.txt", bumped);
        watcher.emit("proj/e.txt", ChangeKind::modified);

        // Unchanged content -> no reconcile change reported...
        CHECK(rec.reconcile_hints().empty());
        // ...but the index's cheap-gate stat was refreshed to the new mtime (the fix).
        CHECK(rec.index().get("proj/e.txt")->mtime_nanos == bumped);
        CHECK(rec.index().get("proj/e.txt")->content_hash == content_hash("E0"));

        // Consequence: a subsequent gated crawl short-circuits (mtime+size match) — no re-read.
        CHECK(rec.crawl(/*gated=*/true).empty());
        CHECK(rec.index().get("proj/e.txt")->content_hash == content_hash("E0"));
    }

    FILESYNC_TEST_MAIN_END();
}
