// NativeWatcher — the real OS watcher backends (RDCW / inotify / FSEvents) on the REAL filesystem:
// per-OS backend smoke on real FS events (create / modify / remove / nested new dirs), hint-driven
// reconciler convergence over NativeFileStore, self-echo suppression under REAL watcher timing,
// bulk git-op convergence (branch-switch storm: the crawl safety net converges whatever hints
// missed), and the degraded-mode contract (registration failure -> latched degraded -> the visible
// watcher.degraded diagnostic; never a silent crawl fall-back). R-FILE-002 / R-QA-013; issue #41.
//
// Determinism note: hint DELIVERY timing is the OS's, so every hint wait is a bounded poll loop and
// every convergence assertion is made on the CRAWL-converged end state (hashes are the truth) —
// hint loss can never fail these tests, only slow them. Informational timings printed to stderr are
// never asserted (R-QA-009: no perf gates on shared runners).

#include "context/editor/filesync/native_watcher.h"

#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/reconciler.h"
#include "context/kernel/event_bus.h"
#include "context/kernel/platform.h"
#include "filesync_test.h"
#include "native_test_support.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace context::editor::filesync;
namespace fs = std::filesystem;

namespace
{

void write_file(const fs::path& path, const std::string& data)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// Bounded wait: run `step` (which returns true when the awaited condition landed) every ~15 ms
// until it succeeds or `timeout` expires. Never asserts — callers decide what a timeout means.
template <class Step>
bool wait_until(Step step, std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (step())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

// Drain the watcher until a hint for `logical` shows up (other pending hints are consumed too —
// each block below waits for one mutation at a time). Also asserts poll-batch dedup: one hint per
// path per drained batch.
bool wait_for_hint(NativeWatcher& watcher, const std::string& logical)
{
    return wait_until(
        [&]
        {
            const std::vector<WatchEvent> batch = watcher.poll();
            std::set<std::string> seen;
            bool found = false;
            for (const WatchEvent& event : batch)
            {
                CHECK(seen.insert(event.path).second); // dedup contract: one hint per path per poll
                if (event.path == logical)
                    found = true;
            }
            return found;
        });
}

bool has_change(const std::vector<ReconcileChange>& changes, const std::string& path)
{
    return std::any_of(changes.begin(), changes.end(),
                       [&](const ReconcileChange& c) { return c.path == path; });
}

} // namespace

int main()
{
    // Every CI platform of this repo (windows / ubuntu / macos) carries a real backend; the
    // unsupported-platform fallback (constructible, permanently degraded, no hints) is the same
    // contract the degraded-registration block asserts below.
    CHECK(NativeWatcher::backend_available());

    // --- 1. per-OS backend smoke: real FS create / modify / remove reach poll() -------------------
    if (NativeWatcher::backend_available())
    {
        nfstest::TempDir tmp("watch-smoke");
        fs::create_directories(tmp.path() / "proj");
        NativeWatcher watcher(tmp.path() / "proj", "proj");
        CHECK(!watcher.degraded());

        // create
        write_file(tmp.path() / "proj" / "a.scene", "A0");
        CHECK(wait_for_hint(watcher, "proj/a.scene"));

        // modify (the create hint was drained above, so this is a fresh hint)
        write_file(tmp.path() / "proj" / "a.scene", "A1");
        CHECK(wait_for_hint(watcher, "proj/a.scene"));

        // remove
        std::error_code ec;
        fs::remove(tmp.path() / "proj" / "a.scene", ec);
        CHECK(wait_for_hint(watcher, "proj/a.scene"));

        // a file inside a directory chain created AFTER the watch started (exercises the
        // new-directory handling: RDCW/FSEvents are natively recursive; inotify adds watches as
        // directories appear and scans them to close the add race)
        write_file(tmp.path() / "proj" / "sub" / "deep" / "b.txt", "B0");
        CHECK(wait_for_hint(watcher, "proj/sub/deep/b.txt"));

        // drained: with no further mutations the queue returns to empty
        CHECK(wait_until([&] { return watcher.poll().empty(); },
                         std::chrono::milliseconds(3000)));
    }

    // --- 2. hint-driven reconciler convergence over real disk (no crawl involved) -----------------
    if (NativeWatcher::backend_available())
    {
        nfstest::TempDir tmp("watch-reconcile");
        fs::create_directories(tmp.path() / "proj");
        NativeFileStore store(tmp.path());
        NativeWatcher watcher(tmp.path() / "proj", "proj");
        context::kernel::SteadyClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(store, watcher, clock, tasks, "proj", "proj/.editor/reconcile-index");
        CHECK(!watcher.degraded());

        // Three external creates converge through hints alone (paths re-hashed UNCONDITIONALLY on
        // hints — no mtime gate — and the content hash decides the change).
        write_file(tmp.path() / "proj" / "one.txt", "1");
        write_file(tmp.path() / "proj" / "two.txt", "2");
        write_file(tmp.path() / "proj" / "nested" / "three.txt", "3");
        std::vector<ReconcileChange> collected;
        CHECK(wait_until(
            [&]
            {
                std::vector<ReconcileChange> got = rec.reconcile_hints();
                collected.insert(collected.end(), got.begin(), got.end());
                return has_change(collected, "proj/one.txt") &&
                       has_change(collected, "proj/two.txt") &&
                       has_change(collected, "proj/nested/three.txt");
            }));
        CHECK(rec.index().get("proj/one.txt")->content_hash == content_hash("1"));
        CHECK(rec.index().get("proj/nested/three.txt")->content_hash == content_hash("3"));

        // External modify -> hint -> reconciled as modified with the new hash.
        write_file(tmp.path() / "proj" / "one.txt", "1'");
        collected.clear();
        CHECK(wait_until(
            [&]
            {
                std::vector<ReconcileChange> got = rec.reconcile_hints();
                collected.insert(collected.end(), got.begin(), got.end());
                return has_change(collected, "proj/one.txt");
            }));
        CHECK(rec.index().get("proj/one.txt")->content_hash == content_hash("1'"));

        // External remove -> hint -> reconciled as removed (index row dropped).
        std::error_code ec;
        fs::remove(tmp.path() / "proj" / "two.txt", ec);
        collected.clear();
        CHECK(wait_until(
            [&]
            {
                std::vector<ReconcileChange> got = rec.reconcile_hints();
                collected.insert(collected.end(), got.begin(), got.end());
                return has_change(collected, "proj/two.txt");
            }));
        CHECK(!rec.index().get("proj/two.txt").has_value());
    }

    // --- 3. self-echo suppression under REAL watcher timing (R-FILE-002 expected-writes table) ----
    if (NativeWatcher::backend_available())
    {
        nfstest::TempDir tmp("watch-selfecho");
        fs::create_directories(tmp.path() / "proj");
        NativeFileStore store(tmp.path());
        NativeWatcher watcher(tmp.path() / "proj", "proj");
        context::kernel::SteadyClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(store, watcher, clock, tasks, "proj", "proj/.editor/reconcile-index");

        // The daemon's own write: the OS watcher WILL see it (temp file + rename + target), but it
        // must never surface as an external change — suppressed by the expected-writes TTL while
        // fresh, and a no-op against the already-updated index after the TTL. Drain well past the
        // 500 ms TTL to prove both halves under real timing.
        CHECK(rec.apply_write("proj/self.txt", "S0"));
        const auto quiet_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        while (std::chrono::steady_clock::now() < quiet_until)
        {
            CHECK(rec.reconcile_hints().empty()); // nothing external happened — nothing surfaces
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        // A genuine LATER external edit of the same path is NOT masked (suppression expired).
        write_file(tmp.path() / "proj" / "self.txt", "S1");
        std::vector<ReconcileChange> collected;
        CHECK(wait_until(
            [&]
            {
                std::vector<ReconcileChange> got = rec.reconcile_hints();
                collected.insert(collected.end(), got.begin(), got.end());
                return has_change(collected, "proj/self.txt");
            }));
        CHECK(rec.index().get("proj/self.txt")->content_hash == content_hash("S1"));
    }

    // --- 4. bulk git-op convergence: branch-switch storm, crawl converges what hints miss ---------
    if (NativeWatcher::backend_available())
    {
        nfstest::TempDir tmp("watch-storm");
        // A pre-existing tree (the "checked-out branch"): 60 files across nested package dirs.
        for (int i = 0; i < 60; ++i)
        {
            const std::string name = "pkg" + std::to_string(i % 5) + "/f" + std::to_string(i) +
                                     ".txt";
            write_file(tmp.path() / "proj" / fs::path(name), "v0-" + std::to_string(i));
        }

        NativeFileStore store(tmp.path());
        NativeWatcher watcher(tmp.path() / "proj", "proj");
        context::kernel::SteadyClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(store, watcher, clock, tasks, "proj", "proj/.editor/reconcile-index");
        (void)rec.crawl(/*gated=*/false); // prime the index with the pre-existing tree
        (void)watcher.poll();             // and discard any hints the priming itself produced

        // The "branch switch": 30 modifies + 15 deletes + 20 creates + a directory rename, applied
        // in a tight burst the way git materializes a checkout.
        for (int i = 0; i < 30; ++i)
            write_file(tmp.path() / "proj" /
                           fs::path("pkg" + std::to_string(i % 5) + "/f" + std::to_string(i) +
                                    ".txt"),
                       "v1-" + std::to_string(i));
        std::error_code ec;
        for (int i = 30; i < 45; ++i)
            fs::remove(tmp.path() / "proj" /
                           fs::path("pkg" + std::to_string(i % 5) + "/f" + std::to_string(i) +
                                    ".txt"),
                       ec);
        for (int i = 0; i < 20; ++i)
            write_file(tmp.path() / "proj" / fs::path("newpkg/n" + std::to_string(i) + ".txt"),
                       "n-" + std::to_string(i));
        // Directory move: children may produce NO per-file events on some platforms — exactly the
        // case the crawl safety net owns.
        fs::rename(tmp.path() / "proj" / "pkg4", tmp.path() / "proj" / "pkg4-renamed", ec);
        CHECK(!ec);

        // Drain hints (bounded); count what the backend delivered, then let the SAFETY NET converge
        // the remainder. Correctness must not depend on hint completeness (R-FILE-002).
        std::vector<ReconcileChange> via_hints;
        (void)wait_until(
            [&]
            {
                std::vector<ReconcileChange> got = rec.reconcile_hints();
                via_hints.insert(via_hints.end(), got.begin(), got.end());
                // 30 modifies land as modified; waiting for those alone keeps the bound tight —
                // the crawl below owns full convergence either way.
                std::size_t modified = 0;
                for (const ReconcileChange& c : via_hints)
                    if (c.type == ChangeType::modified)
                        ++modified;
                return modified >= 30;
            },
            std::chrono::milliseconds(8000));
        CHECK(!via_hints.empty()); // the backend delivered real hints during the storm
        (void)rec.crawl(/*gated=*/false); // the dropped-event safety net converges the rest

        // Converged truth: the index mirrors the on-disk tree exactly (hashes are the truth).
        std::size_t on_disk = 0;
        for (fs::recursive_directory_iterator it(tmp.path() / "proj"), end; it != end; ++it)
        {
            if (!it->is_regular_file())
                continue;
            ++on_disk;
            const std::string logical =
                "proj/" + it->path().lexically_relative(tmp.path() / "proj").generic_string();
            std::ifstream in(it->path(), std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            CHECK(rec.index().get(logical).has_value());
            CHECK(rec.index().get(logical)->content_hash == content_hash(bytes));
        }
        // 60 - 15 deleted + 20 created = 65 survivors (the dir rename relocated 9 of them in place).
        CHECK(on_disk == 65);
        CHECK(rec.index().size() == on_disk); // no stale rows for deleted / moved-away paths
    }

    // --- 5. degraded mode: registration failure latches + emits the visible diagnostic ------------
    {
        nfstest::TempDir tmp("watch-degraded");
        write_file(tmp.path() / "not-a-dir", "x");

        // A missing root and a non-directory root both refuse registration -> degraded, no hints.
        NativeWatcher missing(tmp.path() / "absent", "proj");
        CHECK(missing.degraded());
        CHECK(missing.poll().empty());

        NativeWatcher file_root(tmp.path() / "not-a-dir", "proj");
        CHECK(file_root.degraded());

        // The reconciler surfaces the degraded watcher ONCE, naming the subtree and the crawl
        // fall-back cadence (R-FILE-002: silent fall-back is forbidden).
        NativeFileStore store(tmp.path());
        context::kernel::SteadyClock clock;
        context::kernel::InlineTaskRunner tasks;
        context::kernel::EventBus bus;
        std::vector<std::string> logs;
        bus.subscribe<context::kernel::LogEvent>(
            [&](const context::kernel::LogEvent& event) { logs.push_back(event.message); });
        Reconciler rec(store, file_root, clock, tasks, "proj", "proj/.editor/reconcile-index",
                       &bus);
        (void)rec.reconcile_hints();
        (void)rec.reconcile_hints();
        CHECK(logs.size() == 1);
        CHECK(logs[0].find("watcher.degraded") != std::string::npos);
        CHECK(logs[0].find("proj") != std::string::npos);               // the affected subtree
        CHECK(logs[0].find("re-hash crawl") != std::string::npos);      // the fall-back
        CHECK(logs[0].find("every reconcile pass") != std::string::npos); // ... and its cadence
    }

    // --- 6. informational timings (NEVER asserted — R-QA-009: shared runners, no perf gates) ------
    if (NativeWatcher::backend_available())
    {
        nfstest::TempDir tmp("watch-timing");
        constexpr int kFiles = 2000;
        for (int i = 0; i < kFiles; ++i)
            write_file(tmp.path() / "proj" / fs::path("d" + std::to_string(i % 40)) /
                           fs::path("f" + std::to_string(i) + ".txt"),
                       "seed-" + std::to_string(i));

        NativeFileStore store(tmp.path());
        NativeWatcher watcher(tmp.path() / "proj", "proj");
        context::kernel::SteadyClock clock;
        context::kernel::InlineTaskRunner tasks;
        Reconciler rec(store, watcher, clock, tasks, "proj", "proj/.editor/reconcile-index");
        (void)rec.crawl(/*gated=*/false);
        (void)watcher.poll();

        // Hint-driven single-edit detection (the R-FILE-011(b) incremental-edit external leg).
        write_file(tmp.path() / "proj" / "d0" / "f0.txt", "edited");
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<ReconcileChange> got;
        const bool detected = wait_until(
            [&]
            {
                std::vector<ReconcileChange> step = rec.reconcile_hints();
                got.insert(got.end(), step.begin(), step.end());
                return has_change(got, "proj/d0/f0.txt");
            });
        const double hint_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count();
        CHECK(detected); // correctness (the hint arrived); the TIMING below is informational only

        // The old detection path at the same scale: one gated (mtime/size) crawl over 2000 files.
        const auto c0 = std::chrono::steady_clock::now();
        (void)rec.crawl(/*gated=*/true);
        const double crawl_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - c0)
                                    .count();
        std::fprintf(stderr,
                     "[informational] files=%d watcher-hint detect (incl. OS delivery + poll "
                     "cadence): %.2f ms; gated-crawl detect: %.2f ms\n",
                     kFiles, hint_ms, crawl_ms);
    }

    FILESYNC_TEST_MAIN_END();
}
