// Fault-injection scenario drivers (see fault_scenarios.h). Each driver wires the concrete
// injectable seams — the kernel platform (ManualClock / InlineTaskRunner), the file-sync layer
// (MemoryFileStore + FakeWatcher + Reconciler + WriteQueue / IntentLog) and the bridge event stream
// (EventStream + bounded Subscriber) — under a seeded, reproducible fault schedule and checks the
// invariant the design promises. This is the reusable body of the R-QA-010 harness; the scenario
// tests only fuzz and assert over it.

#include "context/testing/fault_scenarios.h"
#include "context/testing/seeded_rng.h"

#include "context/editor/bridge/event_stream.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/diagnostic.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/intent_log.h"
#include "context/editor/filesync/reconciler.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/platform.h"

#include <optional>
#include <string>
#include <vector>

namespace context::testing
{

namespace fsl = context::editor::filesync;
namespace kern = context::kernel;
namespace br = context::editor::bridge;
namespace ct = context::editor::contract;

namespace
{

// A short, seed-derived lowercase payload; length varies so same-size vs different-size edits both
// occur across the sweep.
std::string content_for(SeededRng& rng)
{
    const std::uint64_t len = rng.range(1, 12);
    std::string out;
    for (std::uint64_t i = 0; i < len; ++i)
        out.push_back(static_cast<char>('a' + rng.bounded(26)));
    return out;
}

// A stable per-scenario HMAC key for the intent log (any consistent key across the two incarnations
// works — begin() and recover() must agree; R-FILE-004).
const char* const kHmacKey = "harness-project-hmac-key-00000001";

} // namespace

CrashScenarioResult run_crash_recovery_scenario(std::uint64_t seed)
{
    SeededRng rng(seed);
    CrashScenarioResult r;

    fsl::MemoryFileStore store;
    kern::ManualClock clock;
    fsl::IntentLog log(store, "proj/.editor", kHmacKey);
    fsl::WriteQueue queue(store, "proj", log, clock);

    const unsigned n = static_cast<unsigned>(rng.range(2, 5));
    r.writes = n;

    // Distinct paths, each pre-seeded with a `prev` value; plan a `next` value per path.
    std::vector<std::string> paths;
    std::vector<std::string> prev(n);
    std::vector<std::string> next(n);
    std::vector<fsl::PlannedWrite> writes;
    for (unsigned i = 0; i < n; ++i)
    {
        std::string path = "proj/f" + std::to_string(i) + ".txt";
        prev[i] = content_for(rng);
        next[i] = content_for(rng);
        store.write(path, prev[i]);
        writes.push_back(
            fsl::PlannedWrite{path, fsl::content_hash(prev[i]), fsl::content_hash(next[i]), next[i]});
        paths.push_back(std::move(path));
    }

    // Arm a one-shot crash at a seeded durable step. The atomic-rename IS the durable commit
    // (write-temp then rename), so a crash between durable steps is a crash on the rename to file i:
    // files 0..i-1 are already committed, i..n-1 are untouched — no file is ever half-written.
    r.crash_index = static_cast<unsigned>(rng.bounded(n));
    store.crash_on_rename_to(paths[r.crash_index]);

    try
    {
        queue.execute("op-crash", writes);
    }
    catch (const fsl::SimulatedCrash&)
    {
        r.observed_crash = true;
    }

    // No torn state: every path holds EITHER its prev OR its target content — never a partial write.
    r.no_torn_state = true;
    for (unsigned i = 0; i < n; ++i)
    {
        const std::optional<std::string> cur = store.read(paths[i]);
        if (!cur || (*cur != prev[i] && *cur != next[i]))
        {
            r.no_torn_state = false;
            break;
        }
    }

    // Recover on a fresh incarnation: resume to completion, nothing lost, no diagnostics (nothing
    // moved on since the crash, so every CAS guard holds).
    fsl::IntentLog log2(store, "proj/.editor", kHmacKey);
    fsl::WriteQueue queue2(store, "proj", log2, clock);
    const std::vector<fsl::Diagnostic> diags = queue2.recover();

    bool all_target = true;
    for (unsigned i = 0; i < n; ++i)
    {
        const std::optional<std::string> cur = store.read(paths[i]);
        if (!cur || *cur != next[i])
        {
            all_target = false;
            break;
        }
    }
    r.recovered = diags.empty() && all_target && log2.pending().empty();

    // Idempotent replay: a second recovery changes nothing.
    const std::vector<fsl::Diagnostic> diags2 = queue2.recover();
    bool still_target = true;
    for (unsigned i = 0; i < n; ++i)
    {
        const std::optional<std::string> cur = store.read(paths[i]);
        if (!cur || *cur != next[i])
        {
            still_target = false;
            break;
        }
    }
    r.idempotent_replay = diags2.empty() && still_target && log2.pending().empty();

    r.detail = "writes=" + std::to_string(n) + " crash_index=" + std::to_string(r.crash_index);
    return r;
}

WatcherScenarioResult run_watcher_fault_scenario(std::uint64_t seed)
{
    SeededRng rng(seed);
    WatcherScenarioResult r;

    fsl::MemoryFileStore store;
    fsl::FakeWatcher watcher;
    kern::ManualClock clock;
    kern::InlineTaskRunner tasks;
    fsl::Reconciler rec(store, watcher, clock, tasks, "proj", "proj/.editor/index");

    const unsigned n = static_cast<unsigned>(rng.range(2, 6));
    r.files = n;
    std::vector<std::string> paths;
    for (unsigned i = 0; i < n; ++i)
    {
        std::string path = "proj/w" + std::to_string(i) + ".txt";
        store.write(path, content_for(rng));
        watcher.emit(path, fsl::ChangeKind::created);
        paths.push_back(std::move(path));
    }
    // Clean initial reconcile: the index now matches disk for every file.
    (void)rec.reconcile_hints();

    // Seeded external mutations: leave / modify / remove each file.
    unsigned muts = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        const std::uint64_t roll = rng.bounded(3); // 0 = leave, 1 = modify, 2 = remove
        if (roll == 1)
        {
            store.write(paths[i], content_for(rng) + "!"); // '!' guarantees a real byte change
            watcher.emit(paths[i], fsl::ChangeKind::modified);
            ++muts;
        }
        else if (roll == 2)
        {
            store.remove(paths[i]);
            watcher.emit(paths[i], fsl::ChangeKind::removed);
            ++muts;
        }
    }
    r.mutations = muts;

    // Apply a seeded combination of watcher faults to the pending hints BEFORE they are drained.
    if (rng.chance(1, 2))
    {
        watcher.duplicate_all();
        r.duplicated = true;
    }
    if (rng.chance(1, 2))
    {
        watcher.reverse();
        r.reordered = true;
    }
    if (rng.chance(1, 3))
    {
        watcher.drop_all();
        r.dropped = true;
    }

    // Drain whatever hints survived (possibly none), then run the FULL re-hash crawl safety net —
    // the design's guarantee is convergence even if EVERY watcher event for a path was lost.
    (void)rec.reconcile_hints();
    (void)rec.crawl(/*gated=*/false);

    // Convergence: the index reflects true on-disk content for every path (present -> hash of the
    // current bytes; absent -> no index row), regardless of which hints were lost/dup'd/reordered.
    r.converged = true;
    for (unsigned i = 0; i < n; ++i)
    {
        const std::optional<std::string> on_disk = store.read(paths[i]);
        const auto row = rec.index().get(paths[i]);
        if (on_disk)
        {
            if (!row || row->content_hash != fsl::content_hash(*on_disk))
            {
                r.converged = false;
                break;
            }
        }
        else if (row.has_value())
        {
            r.converged = false;
            break;
        }
    }

    r.detail = "files=" + std::to_string(n) + " mutations=" + std::to_string(muts) +
               (r.dropped ? " drop" : "") + (r.duplicated ? " dup" : "") +
               (r.reordered ? " reorder" : "");
    return r;
}

SlowClientScenarioResult run_slow_client_scenario(std::uint64_t seed)
{
    SeededRng rng(seed);
    SlowClientScenarioResult r;

    const std::size_t ring_capacity = static_cast<std::size_t>(rng.range(4, 32));
    br::EventStream stream("inc-harness", ring_capacity);

    const std::size_t cap = static_cast<std::size_t>(rng.range(1, 8));
    r.capacity = static_cast<unsigned>(cap);
    br::Subscriber sub({}, cap); // empty topic filter => wants every topic
    stream.add_subscriber(&sub);

    // Flood the subscriber past its capacity WITHOUT draining — the definition of a slow client.
    const unsigned n = static_cast<unsigned>(cap) + static_cast<unsigned>(rng.range(1, 20));
    r.published = n;

    static const char* const kTopics[] = {"files", "derivation", "diagnostics", "log", "clients"};
    bool monotonic = true;
    std::uint64_t expect = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        const std::uint64_t seq = stream.publish(kTopics[rng.bounded(5)], ct::Json::object());
        if (seq != expect + 1)
            monotonic = false;
        expect = seq;
    }
    r.never_blocked = monotonic && stream.last_seq() == n;

    // Overflow (n > cap, subscriber never drained) must raise the gap marker — never block.
    r.gap_flagged = sub.gap();

    // Defined recovery: reset the gap, then either replay-since the last delivered seq (when the
    // ring still holds the missed events) or fall back to a fresh snapshot (always carries lastSeq).
    sub.reset_gap();
    bool gapped = false;
    const std::vector<br::Event> missed = stream.replay_since(sub.last_delivered_seq(), gapped);
    if (gapped)
    {
        const ct::Json snap = stream.snapshot();
        r.recovery_defined = snap.at("lastSeq").as_int() == static_cast<std::int64_t>(stream.last_seq());
    }
    else
    {
        std::uint64_t reached = sub.last_delivered_seq();
        for (const br::Event& e : missed)
            reached = e.seq > reached ? e.seq : reached;
        r.recovery_defined = reached == stream.last_seq();
    }

    r.detail = "published=" + std::to_string(n) + " capacity=" + std::to_string(cap) +
               " ring=" + std::to_string(ring_capacity);
    return r;
}

} // namespace context::testing
