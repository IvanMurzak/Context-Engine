// `context bench …` implementation (see bench_command.h) — the R-FILE-011 benchmark subject over
// the REAL M1 attach pipeline.
//
// Measurement model: every scenario composes the SAME real-disk EditorKernel the daemon uses
// (NativeFileStore + Reconciler + DerivationGraph + bridge Daemon; the proven `context daemon`
// composition) and drives it through the PUBLIC composed API only — no library re-architecture.
// Per-stage attribution comes from two bench-local seams:
//   * TimingFileStore — a FileStore decorator accumulating list()/stat()/read() time, splitting the
//     crawl into its "watch/scan" (enumerate + mtime/size stat gate) and "hash" (read + digest +
//     index bookkeeping) shares;
//   * phase timers around the public pipeline boundaries (crawl -> apply/parse -> run_pass ->
//     settle), mirroring EditorKernel::ingest_external()'s exact sequence stage by stage.
//
// M1 honesty notes (documented in docs/latency-budget-table.md):
//   * "watch" is the reconcile crawl's detection share — the native OS watcher does not exist yet
//     (NullWatcher), so detection is scan-bound by design at M1 (R-FILE-002's safety-net crawl).
//   * "parse" is the M1 placeholder canonicalization (whitespace-normalize + FNV-1a; the M2
//     canonical-JSON serializer replaces the node body behind the same seam).
//   * validate / compose / (template) fan-out stages land with the M2 schema/composition model; the
//     budget table carries them as explicit pending rows, never silently green (R-QA-012 spirit).
//   * DerivationConfig is sized per scenario ON PURPOSE (its header: "a real daemon sizes these to
//     its latency budget — the R-FILE-011 bench task pins the documented maximum dirty-set latency
//     these bound"): attach/edit/bulk/query drain in bulk-sized passes; `sustained` keeps the small
//     library defaults so the R-FILE-013 load-shed/backpressure policy is what gets measured.

#include "context/cli/bench_command.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"
#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/editorkernel/editor_kernel.h"
#include "context/editor/editorkernel/kernel_server.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/platform.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace context::cli
{

namespace fs = std::filesystem;
using editor::contract::ClientHandshake;
using editor::contract::Json;
using editor::bridge::ScopeSet;
using editor::bridge::Session;
using editor::bridge::StartOutcome;
using editor::derivation::canonical_parse;
using editor::editorkernel::EditorKernel;
using editor::editorkernel::EditorKernelConfig;
using editor::editorkernel::EditOutcome;
using editor::editorkernel::KernelServer;
using editor::filesync::FileStat;
using editor::filesync::FileStore;
using editor::filesync::NativeFileStore;
using editor::filesync::NullWatcher;
using editor::filesync::ReconcileChange;

namespace
{

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

std::optional<std::string> flag_value(const std::vector<std::string>& args, const std::string& name)
{
    const std::string prefix = "--" + name;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == prefix && i + 1 < args.size())
            return args[i + 1];
        const std::string eq = prefix + "=";
        if (args[i].rfind(eq, 0) == 0)
            return args[i].substr(eq.size());
    }
    return std::nullopt;
}

std::uint64_t flag_u64(const std::vector<std::string>& args, const std::string& name,
                       std::uint64_t fallback)
{
    const std::optional<std::string> v = flag_value(args, name);
    if (!v.has_value())
        return fallback;
    // Digits only: std::stoull ACCEPTS a leading '-' and value-wraps (2^64 - n), which would turn
    // e.g. `--samples -5` into a ~1.8e19-element reservation instead of the graceful fallback every
    // other malformed value gets. Non-digit tokens fall back exactly like the catch below.
    if (v->empty() || v->find_first_not_of("0123456789") != std::string::npos)
        return fallback;
    try
    {
        return static_cast<std::uint64_t>(std::stoull(*v)); // still throws on > 2^64 digit strings
    }
    catch (const std::exception&)
    {
        return fallback;
    }
}

using SteadyPoint = std::chrono::steady_clock::time_point;

SteadyPoint bench_now()
{
    return std::chrono::steady_clock::now();
}

double seconds_between(SteadyPoint a, SteadyPoint b)
{
    return std::chrono::duration<double>(b - a).count();
}

double millis_between(SteadyPoint a, SteadyPoint b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Nearest-rank percentile over an UNSORTED sample vector (sorts a copy). p in [0, 100].
double percentile_ms(std::vector<double> samples, double p)
{
    if (samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const double rank = (p / 100.0) * static_cast<double>(samples.size() - 1);
    const auto lo = static_cast<std::size_t>(rank);
    const std::size_t hi = std::min(lo + 1, samples.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return samples[lo] + (samples[hi] - samples[lo]) * frac;
}

int emit_error(std::string& out_json, const std::string& message)
{
    Json err = Json::object();
    err.set("error", Json(message));
    out_json = err.dump(2);
    std::fprintf(stderr, "[bench] error: %s\n", message.c_str());
    return 2;
}

// R-FILE-011(a): fresh attach reports progress — one machine-readable event on stderr per slice.
// (String-built rather than printf-formatted: 64-bit format specifiers are not portable across the
// three CI toolchains under -Werror.) total == 0 means "unknown".
void emit_progress(const char* stage, std::uint64_t done, std::uint64_t total)
{
    const std::string line = std::string("{\"event\": \"bench.progress\", \"stage\": \"") + stage +
                             "\", \"done\": " + std::to_string(done) +
                             ", \"total\": " + std::to_string(total) + "}\n";
    std::fputs(line.c_str(), stderr);
}

// ---------------------------------------------------------------------------------------------
// TimingFileStore — the bench-local decorator splitting crawl time into stage shares
// ---------------------------------------------------------------------------------------------

class TimingFileStore final : public FileStore
{
public:
    explicit TimingFileStore(FileStore& inner, std::uint64_t progress_every = 0,
                             const char* progress_stage = "scan")
        : inner_(inner), progress_every_(progress_every), progress_stage_(progress_stage)
    {
    }

    struct Counters
    {
        double list_seconds = 0.0;
        double stat_seconds = 0.0;
        double read_seconds = 0.0;
        std::uint64_t lists = 0;
        std::uint64_t stats = 0;
        std::uint64_t reads = 0;
        std::uint64_t read_bytes = 0;
    };

    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

    [[nodiscard]] bool exists(std::string_view path) const override { return inner_.exists(path); }

    [[nodiscard]] std::optional<std::string> read(std::string_view path) const override
    {
        const SteadyPoint t0 = bench_now();
        std::optional<std::string> out = inner_.read(path);
        counters_.read_seconds += seconds_between(t0, bench_now());
        ++counters_.reads;
        if (out.has_value())
            counters_.read_bytes += out->size();
        if (progress_every_ != 0 && counters_.reads % progress_every_ == 0)
            emit_progress(progress_stage_, counters_.reads, 0); // total unknown mid-scan
        return out;
    }

    [[nodiscard]] std::optional<FileStat> stat(std::string_view path) const override
    {
        const SteadyPoint t0 = bench_now();
        std::optional<FileStat> out = inner_.stat(path);
        counters_.stat_seconds += seconds_between(t0, bench_now());
        ++counters_.stats;
        return out;
    }

    [[nodiscard]] std::vector<std::string> list(std::string_view dir) const override
    {
        const SteadyPoint t0 = bench_now();
        std::vector<std::string> out = inner_.list(dir);
        counters_.list_seconds += seconds_between(t0, bench_now());
        ++counters_.lists;
        return out;
    }

    bool write(std::string_view path, std::string_view data) override
    {
        return inner_.write(path, data);
    }
    bool rename(std::string_view from, std::string_view to) override
    {
        return inner_.rename(from, to);
    }
    bool remove(std::string_view path) override { return inner_.remove(path); }
    void fsync(std::string_view path) override { inner_.fsync(path); }

private:
    FileStore& inner_;
    // Accumulators are mutable because FileStore's read paths are const (the decorated store is
    // logically read-observing here, not mutated).
    mutable Counters counters_;
    std::uint64_t progress_every_ = 0;
    const char* progress_stage_ = "scan";
};

// ---------------------------------------------------------------------------------------------
// Shared composition — the daemon's real-disk EditorKernel shape (daemon_command.cpp parity)
// ---------------------------------------------------------------------------------------------

constexpr const char* kFilesyncRoot = "project";                            // gen_corpus layout
constexpr const char* kIndexPath = "project/.editor/reconcile-index";       // control-path-skipped
// Bulk-sized derivation config for whole-corpus scenarios: drain everything in each pass (the
// overloaded load-shed path re-scans pending per pass, so a 100k flood under the tiny library
// defaults would measure the shed policy's overhead, not attach throughput — `sustained` measures
// the shed policy on purpose, with the library defaults).
constexpr std::size_t kBulkWatermark = 1u << 30;

struct BenchRig
{
    fs::path corpus;
    NativeFileStore native;
    TimingFileStore store;
    NullWatcher watcher;
    context::kernel::SteadyClock clock;
    context::kernel::InlineTaskRunner tasks;
    EditorKernel kernel;
    KernelServer server; // registered as method backend before start() (kernel_server.h contract)

    BenchRig(const fs::path& corpus_dir, std::size_t high_watermark, std::size_t max_batch,
             std::uint64_t progress_every)
        : corpus(corpus_dir), native(corpus_dir), store(native, progress_every),
          kernel(store, watcher, clock, tasks, make_config(corpus_dir, high_watermark, max_batch)),
          server(kernel)
    {
    }

    static EditorKernelConfig make_config(const fs::path& corpus_dir, std::size_t high_watermark,
                                          std::size_t max_batch)
    {
        EditorKernelConfig cfg;
        cfg.project_root = corpus_dir;
        cfg.filesync_root = kFilesyncRoot;
        cfg.index_path = kIndexPath;
        cfg.derivation.high_watermark = high_watermark;
        cfg.derivation.max_batch_per_pass = max_batch;
        return cfg;
    }
};

// The corpus contract: `<corpus>/project/` is the authored root (gen_corpus.py layout).
std::optional<std::string> check_corpus(const fs::path& corpus)
{
    std::error_code ec;
    if (!fs::is_directory(corpus, ec))
        return "corpus directory does not exist: " + corpus.string();
    if (!fs::is_directory(corpus / kFilesyncRoot, ec))
        return "corpus has no 'project/' authored root (expected gen_corpus.py layout): " +
               (corpus / kFilesyncRoot).string();
    return std::nullopt;
}

// Ingest `changes` through the graph exactly the way EditorKernel::ingest_external() does (read the
// current bytes, apply into the graph), but with the read + parse phases individually timed.
struct IngestTimings
{
    double read_seconds = 0.0;
    double parse_seconds = 0.0;
    std::uint64_t applied = 0;
};

IngestTimings ingest_changes(BenchRig& rig, const std::vector<ReconcileChange>& changes,
                             std::uint64_t progress_every)
{
    IngestTimings t;
    const std::uint64_t total = changes.size();
    for (const ReconcileChange& change : changes)
    {
        std::string bytes;
        if (change.type != editor::filesync::ChangeType::removed)
        {
            const SteadyPoint r0 = bench_now();
            std::optional<std::string> data = rig.store.read(change.path);
            t.read_seconds += seconds_between(r0, bench_now());
            if (!data.has_value())
                continue; // vanished between crawl and read — the real path tolerates this too
            bytes = std::move(*data);
        }
        const SteadyPoint p0 = bench_now();
        (void)rig.kernel.graph().apply(change, bytes);
        t.parse_seconds += seconds_between(p0, bench_now());
        ++t.applied;
        if (progress_every != 0 && t.applied % progress_every == 0)
            emit_progress("parse", t.applied, total);
    }
    return t;
}

// Drain every pending dirty node; returns (wall seconds, passes run).
std::pair<double, std::uint64_t> drain_passes(BenchRig& rig)
{
    const SteadyPoint t0 = bench_now();
    std::uint64_t passes = 0;
    while (rig.kernel.graph().pending_count() > 0)
    {
        (void)rig.kernel.graph().run_pass();
        ++passes;
    }
    return {seconds_between(t0, bench_now()), passes};
}

// Collect the corpus' scene paths (deterministic order — NativeFileStore::list is sorted).
std::vector<std::string> scene_paths(BenchRig& rig)
{
    std::vector<std::string> scenes;
    for (const std::string& path : rig.store.list(kFilesyncRoot))
    {
        if (path.size() > 11 && path.compare(path.size() - 11, 11, ".scene.json") == 0)
            scenes.push_back(path);
    }
    return scenes;
}

// Synthesize `created` changes for every authored file — index-INDEPENDENT world population for
// scenarios that need a corpus-sized derived world regardless of persisted-index state (a current
// index would make the reconcile crawl honestly report "no changes", which is correct for attach
// but useless for populating a query target). Skips the `.editor/` control dir like the reconciler.
std::vector<ReconcileChange> synthesize_created_changes(BenchRig& rig)
{
    std::vector<ReconcileChange> changes;
    for (std::string& path : rig.store.list(kFilesyncRoot))
    {
        if (path.find("/.editor/") != std::string::npos)
            continue;
        ReconcileChange c;
        c.path = std::move(path);
        c.type = editor::filesync::ChangeType::created;
        changes.push_back(std::move(c));
    }
    return changes;
}

// Deterministically mutate a generated scene's bytes so its CANONICAL form changes (a whitespace
// tweak would be normalized away by the M1 canonicalizer and memoized to a no-op). Every generated
// scene carries the root SceneSettings' `"timeScale": 1`.
std::optional<std::string> mutate_scene(const std::string& bytes, std::uint64_t nonce)
{
    static constexpr std::string_view kNeedle = "\"timeScale\": 1";
    const std::size_t at = bytes.find(kNeedle);
    if (at == std::string::npos)
        return std::nullopt;
    std::string out = bytes;
    out.replace(at, kNeedle.size(), "\"timeScale\": 1.0" + std::to_string(nonce % 1000000));
    return out;
}

Json counters_json(const TimingFileStore::Counters& c)
{
    Json out = Json::object();
    out.set("listSeconds", Json(c.list_seconds));
    out.set("statSeconds", Json(c.stat_seconds));
    out.set("readSeconds", Json(c.read_seconds));
    out.set("lists", Json(c.lists));
    out.set("stats", Json(c.stats));
    out.set("reads", Json(c.reads));
    out.set("readBytes", Json(c.read_bytes));
    return out;
}

// ---------------------------------------------------------------------------------------------
// attach — R-FILE-011(a): fresh (parse+canonicalize+hash-throughput-bounded, with progress) and
// index-warm (mtime/size-gated scan) attach over the real boot + crawl + derive + settle loop.
// ---------------------------------------------------------------------------------------------

int bench_attach(const std::vector<std::string>& args, const fs::path& corpus,
                 std::string& out_json)
{
    const std::string mode = flag_value(args, "mode").value_or("fresh");
    if (mode != "fresh" && mode != "warm")
        return emit_error(out_json, "attach --mode must be 'fresh' or 'warm' (got '" + mode + "')");
    const bool fresh = mode == "fresh";
    const std::uint64_t threads_requested = flag_u64(args, "threads", 0);
    const std::uint64_t progress_every = flag_u64(args, "progress-every", 5000);
    const std::size_t high_watermark =
        static_cast<std::size_t>(flag_u64(args, "high-watermark", kBulkWatermark));
    const std::size_t max_batch =
        static_cast<std::size_t>(flag_u64(args, "max-batch", kBulkWatermark));

    const fs::path index_file = corpus / kIndexPath;
    std::error_code ec;
    if (fresh)
    {
        // A fresh attach measures the no-index cold path; drop any persisted index first.
        fs::remove(index_file, ec);
    }
    else if (!fs::exists(index_file, ec))
    {
        return emit_error(out_json,
                          "attach --mode warm requires a persisted reconcile index (run a fresh "
                          "attach first): " + index_file.string());
    }

    BenchRig rig(corpus, high_watermark, max_batch, progress_every);

    // Boot: single-instance lock -> R-FILE-004 recovery pass -> warm index load (reconciler.attach).
    const SteadyPoint t_boot0 = bench_now();
    const StartOutcome outcome = rig.kernel.start(ScopeSet::all());
    const double boot_seconds = seconds_between(t_boot0, bench_now());
    if (outcome != StartOutcome::booted)
        return emit_error(out_json, outcome == StartOutcome::attach
                                        ? "an EditorKernel is already live on this corpus"
                                        : "daemon boot failed: " +
                                              rig.kernel.daemon().error_message());
    const std::uint64_t index_entries = rig.kernel.reconciler().index().size();

    // Scan: the reconcile crawl. Fresh = ungated full read+hash crawl (every file is new);
    // warm = the mtime/size-gated scan (unchanged files are skipped without a read).
    const TimingFileStore::Counters before_scan = rig.store.counters();
    const SteadyPoint t_scan0 = bench_now();
    const std::vector<ReconcileChange> changes = rig.kernel.reconciler().crawl(/*gated=*/!fresh);
    const double scan_seconds = seconds_between(t_scan0, bench_now());
    const TimingFileStore::Counters after_scan = rig.store.counters();
    const double watch_seconds = (after_scan.list_seconds - before_scan.list_seconds) +
                                 (after_scan.stat_seconds - before_scan.stat_seconds);
    const double scan_read_seconds = after_scan.read_seconds - before_scan.read_seconds;
    const double hash_seconds = std::max(0.0, scan_seconds - watch_seconds - scan_read_seconds);

    // Parse (+canonicalize +canonical-hash) and instantiate: the ingest_external sequence, staged.
    const IngestTimings ingest = ingest_changes(rig, changes, progress_every);
    const auto [instantiate_seconds, passes] = drain_passes(rig);

    // Fan-out: the R-BRIDGE-008 quiescence publish (event-stream fan-out to subscribed clients;
    // template-instance fan-out is an M2 composition stage — see docs/latency-budget-table.md).
    const SteadyPoint t_settle0 = bench_now();
    const std::uint64_t generation = rig.kernel.settle();
    const double fanout_seconds = seconds_between(t_settle0, bench_now());

    // Persist the index so a subsequent warm attach is real (part of the attach work, timed).
    const SteadyPoint t_index0 = bench_now();
    const bool index_saved = rig.kernel.reconciler().save_index();
    const double index_save_seconds = seconds_between(t_index0, bench_now());

    const double wall_seconds = boot_seconds + scan_seconds + ingest.read_seconds +
                                ingest.parse_seconds + instantiate_seconds + fanout_seconds +
                                index_save_seconds;
    const std::uint64_t world_entities = rig.kernel.world().alive_count();
    rig.kernel.stop();

    Json stages = Json::object();
    stages.set("boot_seconds", Json(boot_seconds));
    stages.set("watch_seconds", Json(watch_seconds));
    stages.set("hash_seconds", Json(hash_seconds + scan_read_seconds));
    stages.set("hash_read_share_seconds", Json(scan_read_seconds));
    stages.set("parse_seconds", Json(ingest.parse_seconds));
    stages.set("parse_read_share_seconds", Json(ingest.read_seconds));
    stages.set("validate_seconds", Json(nullptr));    // schema validation lands M2
    stages.set("compose_seconds", Json(nullptr));     // scene composition lands M2
    stages.set("instantiate_seconds", Json(instantiate_seconds));
    stages.set("fanout_seconds", Json(fanout_seconds));
    stages.set("index_save_seconds", Json(index_save_seconds));

    Json out = Json::object();
    out.set("scenario", Json(std::string("attach")));
    out.set("mode", Json(mode));
    out.set("wall_seconds", Json(wall_seconds));
    out.set("scan_seconds", Json(scan_seconds));
    out.set("parse_seconds", Json(ingest.parse_seconds));
    out.set("instantiate_seconds", Json(instantiate_seconds));
    out.set("stages", std::move(stages));
    out.set("files_changed", Json(static_cast<std::uint64_t>(changes.size())));
    out.set("files_applied", Json(ingest.applied));
    out.set("index_entries_loaded", Json(index_entries));
    out.set("index_saved", Json(index_saved));
    out.set("world_entities", Json(world_entities));
    out.set("generation", Json(generation));
    out.set("passes", Json(passes));
    out.set("threads_requested", Json(threads_requested));
    // The M1 crawl + derivation run serially on the inline task runner — recorded honestly so the
    // "fully parallel scan" R-FILE-011(a) target's gap stays visible in the results.
    out.set("threads_effective", Json(static_cast<std::uint64_t>(1)));
    out.set("store", counters_json(rig.store.counters()));
    Json cfg = Json::object();
    cfg.set("high_watermark", Json(static_cast<std::uint64_t>(high_watermark)));
    cfg.set("max_batch_per_pass", Json(static_cast<std::uint64_t>(max_batch)));
    out.set("derivation_config", std::move(cfg));
    out_json = out.dump(2);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// edit — R-FILE-011(b): one authored-file change -> updated derived state, both write paths.
// ---------------------------------------------------------------------------------------------

int bench_edit(const std::vector<std::string>& args, const fs::path& corpus, std::string& out_json)
{
    const std::uint64_t seed = flag_u64(args, "seed", 1);

    BenchRig rig(corpus, kBulkWatermark, kBulkWatermark, 0);
    if (rig.kernel.start(ScopeSet::all()) != StartOutcome::booted)
        return emit_error(out_json, "daemon boot failed: " + rig.kernel.daemon().error_message());

    // Untimed prime: bring the reconcile index current so the timed detection below measures the
    // steady-state gated scan, not a cold index build. Gated on purpose: the stat-scan converges
    // any drift (our mutations always change size/mtime) without re-hashing the whole corpus.
    (void)rig.kernel.reconciler().crawl(/*gated=*/true);

    const std::vector<std::string> scenes = scene_paths(rig);
    if (scenes.size() < 2)
    {
        rig.kernel.stop();
        return emit_error(out_json, "corpus has fewer than 2 scene files");
    }

    // Leg A — the daemon-initiated ("CLI-verb") write path: edit_file -> own-write barrier.
    const std::string& cli_path = scenes[seed % scenes.size()];
    const std::optional<std::string> cli_bytes = rig.store.read(cli_path);
    std::optional<std::string> cli_mutated =
        cli_bytes.has_value() ? mutate_scene(*cli_bytes, seed) : std::nullopt;
    if (!cli_mutated.has_value())
    {
        rig.kernel.stop();
        return emit_error(out_json, "could not mutate scene (no timeScale field?): " + cli_path);
    }
    const SteadyPoint a0 = bench_now();
    const EditOutcome edit = rig.kernel.edit_file(cli_path, *cli_mutated, ScopeSet::all());
    if (!edit.ok)
    {
        rig.kernel.stop();
        return emit_error(out_json, "edit_file failed: " + edit.error_code);
    }
    (void)rig.kernel.await_hash(edit.ticket.canonical_hash);
    const double edit_cli_ms = millis_between(a0, bench_now());
    const bool cli_reflected = rig.kernel.graph().reflects_hash(edit.ticket.canonical_hash);

    // Leg B — an EXTERNAL raw edit: mutate on disk behind the kernel's back, then measure the whole
    // detect (gated crawl) -> read -> parse -> derive incremental path. With no native watcher at
    // M1, detection is the mtime/size-gated scan — the honest current bound of the 100 ms target.
    const std::string& ext_path = scenes[(seed + 1) % scenes.size()];
    const std::optional<std::string> ext_bytes = rig.store.read(ext_path);
    std::optional<std::string> ext_mutated =
        ext_bytes.has_value() ? mutate_scene(*ext_bytes, seed ^ 0x5a5a5a5aULL) : std::nullopt;
    if (!ext_mutated.has_value())
    {
        rig.kernel.stop();
        return emit_error(out_json, "could not mutate scene (no timeScale field?): " + ext_path);
    }
    const std::uint64_t expected_hash = canonical_parse(*ext_mutated).canonical_hash;
    if (!rig.native.write(ext_path, *ext_mutated))
    {
        rig.kernel.stop();
        return emit_error(out_json, "raw write failed: " + ext_path);
    }

    const SteadyPoint b0 = bench_now();
    const std::vector<ReconcileChange> detected = rig.kernel.reconciler().crawl(/*gated=*/true);
    const double detect_ms = millis_between(b0, bench_now());
    const IngestTimings ingest = ingest_changes(rig, detected, 0);
    (void)drain_passes(rig);
    const double latency_ms = millis_between(b0, bench_now());
    const bool ext_reflected = rig.kernel.graph().reflects_hash(expected_hash);

    (void)rig.kernel.settle();
    (void)rig.kernel.reconciler().save_index();
    rig.kernel.stop();

    Json out = Json::object();
    out.set("scenario", Json(std::string("edit")));
    out.set("latency_ms", Json(latency_ms)); // primary: the external-change incremental path
    out.set("detect_ms", Json(detect_ms));
    out.set("apply_ms", Json((ingest.read_seconds + ingest.parse_seconds) * 1000.0));
    out.set("edit_cli_verb_ms", Json(edit_cli_ms));
    out.set("external_reflected", Json(ext_reflected));
    out.set("cli_verb_reflected", Json(cli_reflected));
    out.set("changes_detected", Json(static_cast<std::uint64_t>(detected.size())));
    out.set("external_path", Json(ext_path));
    out.set("cli_verb_path", Json(cli_path));
    out_json = out.dump(2);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// bulk — R-FILE-011(c): a branch-switch-class burst converges through one coalesced cycle.
// ---------------------------------------------------------------------------------------------

int bench_bulk(const std::vector<std::string>& args, const fs::path& corpus, std::string& out_json)
{
    const std::uint64_t seed = flag_u64(args, "seed", 1);
    const std::uint64_t count = flag_u64(args, "count", 2000);

    BenchRig rig(corpus, kBulkWatermark, kBulkWatermark, 0);
    if (rig.kernel.start(ScopeSet::all()) != StartOutcome::booted)
        return emit_error(out_json, "daemon boot failed: " + rig.kernel.daemon().error_message());
    (void)rig.kernel.reconciler().crawl(/*gated=*/true); // untimed index prime (stat-only scan)

    const std::vector<std::string> scenes = scene_paths(rig);
    if (scenes.empty())
    {
        rig.kernel.stop();
        return emit_error(out_json, "corpus has no scene files");
    }
    const std::uint64_t k = std::min<std::uint64_t>(count, scenes.size());

    // Mutate K distinct scenes on disk behind the kernel's back (the bulk external burst). The
    // nonce folds in the seed so distinct harness runs converge distinct content.
    std::uint64_t mutated = 0;
    for (std::uint64_t i = 0; i < k; ++i)
    {
        const std::string& path = scenes[(seed + i * 2654435761ULL) % scenes.size()];
        const std::optional<std::string> bytes = rig.store.read(path);
        if (!bytes.has_value())
            continue;
        const std::optional<std::string> next = mutate_scene(*bytes, seed + i);
        if (!next.has_value())
            continue;
        if (rig.native.write(path, *next))
            ++mutated;
    }

    const SteadyPoint t0 = bench_now();
    const std::vector<ReconcileChange> detected = rig.kernel.reconciler().crawl(/*gated=*/true);
    const double detect_seconds = seconds_between(t0, bench_now());
    const IngestTimings ingest = ingest_changes(rig, detected, 0);
    const auto [derive_seconds, passes] = drain_passes(rig);
    const std::uint64_t generation = rig.kernel.settle();
    const double wall_seconds = seconds_between(t0, bench_now());

    (void)rig.kernel.reconciler().save_index();
    rig.kernel.stop();

    Json out = Json::object();
    out.set("scenario", Json(std::string("bulk")));
    out.set("wall_seconds", Json(wall_seconds));
    out.set("detect_seconds", Json(detect_seconds));
    out.set("apply_seconds", Json(ingest.read_seconds + ingest.parse_seconds));
    out.set("derive_seconds", Json(derive_seconds));
    out.set("files_mutated", Json(mutated));
    out.set("changes_detected", Json(static_cast<std::uint64_t>(detected.size())));
    out.set("passes", Json(passes));
    out.set("generation", Json(generation));
    out_json = out.dump(2);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// query — the R-BRIDGE-008 session-query budget (<= 5 ms p99 local), measured at the daemon's
// bounded service point: the full JSON-RPC dispatch (parse -> scope check -> KernelServer backend
// -> derived-world read -> envelope serialize) against an attached session, minus socket I/O.
// ---------------------------------------------------------------------------------------------

int bench_query(const std::vector<std::string>& args, const fs::path& corpus,
                std::string& out_json)
{
    const std::uint64_t seed = flag_u64(args, "seed", 1);
    const std::uint64_t samples = std::max<std::uint64_t>(1, flag_u64(args, "samples", 2000));

    BenchRig rig(corpus, kBulkWatermark, kBulkWatermark, 0);
    if (rig.kernel.start(ScopeSet::all()) != StartOutcome::booted)
        return emit_error(out_json, "daemon boot failed: " + rig.kernel.daemon().error_message());

    // Untimed setup: populate the derived world (the queries must read a real, corpus-sized world).
    // Index-independent on purpose — a current persisted index would make a crawl report nothing.
    const std::vector<ReconcileChange> changes = synthesize_created_changes(rig);
    (void)ingest_changes(rig, changes, 0);
    (void)drain_passes(rig);
    (void)rig.kernel.settle();

    const std::vector<std::string> scenes = scene_paths(rig);
    if (scenes.empty())
    {
        rig.kernel.stop();
        return emit_error(out_json, "corpus has no scene files");
    }

    // Attach a read-scoped session over the capability handshake — the session the queries bill to.
    ClientHandshake client;
    client.protocol_major = editor::contract::kProtocolMajor;
    client.capabilities = {"describe"};
    auto attached = rig.kernel.attach(client, ScopeSet::read_query());
    if (std::holds_alternative<editor::contract::Envelope>(attached))
    {
        rig.kernel.stop();
        return emit_error(out_json, "handshake attach failed");
    }
    Session session = std::get<Session>(attached);

    const editor::bridge::Dispatcher& dispatcher = rig.kernel.daemon().dispatcher();
    std::vector<double> latencies;
    latencies.reserve(samples);
    std::uint64_t ok = 0;
    for (std::uint64_t i = 0; i < samples; ++i)
    {
        const std::string& path = scenes[(seed + i * 2654435761ULL) % scenes.size()];
        Json params = Json::object();
        params.set("path", Json(path));
        Json req = Json::object();
        req.set("jsonrpc", Json(std::string("2.0")));
        req.set("id", Json(static_cast<std::int64_t>(i + 1)));
        req.set("method", Json(std::string("query")));
        req.set("params", std::move(params));
        const std::string request = req.dump(0);

        const SteadyPoint t0 = bench_now();
        const std::string response = dispatcher.handle(request, session);
        latencies.push_back(millis_between(t0, bench_now()));
        if (response.find("\"present\": true") != std::string::npos ||
            response.find("\"present\":true") != std::string::npos)
            ++ok;
    }
    const std::uint64_t world_entities = rig.kernel.world().alive_count();
    rig.kernel.stop();

    double mean = 0.0;
    for (const double v : latencies)
        mean += v;
    mean /= static_cast<double>(latencies.size());

    Json out = Json::object();
    out.set("scenario", Json(std::string("query")));
    out.set("p50_ms", Json(percentile_ms(latencies, 50.0)));
    out.set("p95_ms", Json(percentile_ms(latencies, 95.0)));
    out.set("p99_ms", Json(percentile_ms(latencies, 99.0)));
    out.set("max_ms", Json(*std::max_element(latencies.begin(), latencies.end())));
    out.set("mean_ms", Json(mean));
    out.set("samples", Json(static_cast<std::uint64_t>(latencies.size())));
    out.set("present_hits", Json(ok));
    out.set("world_entities", Json(world_entities));
    out_json = out.dump(2);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// sustained — R-FILE-013: sustained write load vs derivation-side backpressure. Writes outpace
// passes on purpose (the small LIBRARY-default coalescing config), and the measured maximum
// dirty-set latency is the documented R-FILE-013 number the budget table carries.
// ---------------------------------------------------------------------------------------------

int bench_sustained(const std::vector<std::string>& args, const fs::path& corpus,
                    std::string& out_json)
{
    const std::uint64_t writes = std::max<std::uint64_t>(16, flag_u64(args, "writes", 2000));
    const std::uint64_t sample_every = std::max<std::uint64_t>(1, flag_u64(args, "sample-every", 64));
    const std::uint64_t pump_every = std::max<std::uint64_t>(1, flag_u64(args, "pump-every", 8));
    // The library defaults ARE the subject here (the load-shed/coalescing policy under pressure).
    const editor::derivation::DerivationConfig defaults{};
    const std::size_t high_watermark = static_cast<std::size_t>(
        flag_u64(args, "high-watermark", defaults.high_watermark));
    const std::size_t max_batch = static_cast<std::size_t>(
        flag_u64(args, "max-batch", defaults.max_batch_per_pass));

    BenchRig rig(corpus, high_watermark, max_batch, 0);
    if (rig.kernel.start(ScopeSet::all()) != StartOutcome::booted)
        return emit_error(out_json, "daemon boot failed: " + rig.kernel.daemon().error_message());

    // A per-invocation nonce keeps repeated harness runs writing genuinely NEW content (identical
    // bytes would memoize to no-ops and measure nothing).
    const std::uint64_t nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    struct Sample
    {
        std::uint64_t hash = 0;
        SteadyPoint written{};
        double latency_ms = -1.0;
    };
    std::vector<Sample> tracked;
    tracked.reserve(writes / sample_every + 1);

    std::size_t max_queue_depth = 0;
    std::uint64_t overload_transitions = 0;
    bool was_overloaded = false;
    std::uint64_t passes = 0;

    const auto observe = [&]
    {
        const editor::derivation::BackpressureSignal& sig = rig.kernel.graph().backpressure();
        max_queue_depth = std::max(max_queue_depth, sig.queue_depth);
        if (sig.overloaded && !was_overloaded)
            ++overload_transitions;
        was_overloaded = sig.overloaded;
    };
    const auto reap_reflected = [&](SteadyPoint at)
    {
        for (Sample& s : tracked)
        {
            if (s.latency_ms < 0.0 && rig.kernel.graph().reflects_hash(s.hash))
                s.latency_ms = millis_between(s.written, at);
        }
    };

    const SteadyPoint t0 = bench_now();
    for (std::uint64_t i = 0; i < writes; ++i)
    {
        const std::string path =
            "project/.bench-sustained/w" + std::to_string(i % 512) + ".scene.json";
        const std::string content = "{ \"benchNonce\": " + std::to_string(nonce + i) + " }";
        const EditOutcome out = rig.kernel.edit_file(path, content, ScopeSet::all());
        if (!out.ok)
        {
            rig.kernel.stop();
            return emit_error(out_json, "sustained edit_file failed: " + out.error_code);
        }
        if (i % sample_every == 0)
            tracked.push_back(Sample{out.ticket.canonical_hash, bench_now(), -1.0});
        observe();
        if (i % pump_every == pump_every - 1)
        {
            (void)rig.kernel.graph().run_pass();
            ++passes;
            reap_reflected(bench_now());
            observe();
        }
    }
    // Catch-up: drain to quiescence, reaping sample latencies after every pass (R-FILE-013's
    // "completion signal" is the settle that follows).
    while (rig.kernel.graph().pending_count() > 0)
    {
        (void)rig.kernel.graph().run_pass();
        ++passes;
        reap_reflected(bench_now());
        observe();
    }
    const std::uint64_t generation = rig.kernel.settle();
    const double wall_seconds = seconds_between(t0, bench_now());

    // Untimed cleanup: remove the synthetic write targets and fold the removals back in, so later
    // scenarios (and the persisted index) see the corpus, not the load fixture.
    for (std::uint64_t i = 0; i < std::min<std::uint64_t>(writes, 512); ++i)
        (void)rig.native.remove("project/.bench-sustained/w" + std::to_string(i) + ".scene.json");
    const std::vector<ReconcileChange> removals = rig.kernel.reconciler().crawl(/*gated=*/true);
    (void)ingest_changes(rig, removals, 0);
    (void)drain_passes(rig);
    (void)rig.kernel.settle();
    (void)rig.kernel.reconciler().save_index();
    rig.kernel.stop();

    std::vector<double> reflected;
    for (const Sample& s : tracked)
        if (s.latency_ms >= 0.0)
            reflected.push_back(s.latency_ms);
    if (reflected.empty())
        return emit_error(out_json, "sustained scenario reflected no sampled writes");

    Json out = Json::object();
    out.set("scenario", Json(std::string("sustained")));
    out.set("dirty_latency_max_ms", Json(*std::max_element(reflected.begin(), reflected.end())));
    out.set("dirty_latency_p99_ms", Json(percentile_ms(reflected, 99.0)));
    out.set("dirty_latency_p50_ms", Json(percentile_ms(reflected, 50.0)));
    out.set("wall_seconds", Json(wall_seconds));
    out.set("writes", Json(writes));
    out.set("writes_per_second", Json(static_cast<double>(writes) / std::max(wall_seconds, 1e-9)));
    out.set("samples_reflected", Json(static_cast<std::uint64_t>(reflected.size())));
    out.set("samples_tracked", Json(static_cast<std::uint64_t>(tracked.size())));
    out.set("max_queue_depth", Json(static_cast<std::uint64_t>(max_queue_depth)));
    out.set("overload_transitions", Json(overload_transitions));
    out.set("passes", Json(passes));
    out.set("generation", Json(generation));
    Json cfg = Json::object();
    cfg.set("high_watermark", Json(static_cast<std::uint64_t>(high_watermark)));
    cfg.set("max_batch_per_pass", Json(static_cast<std::uint64_t>(max_batch)));
    out.set("derivation_config", std::move(cfg));
    out_json = out.dump(2);
    return 0;
}

} // namespace

int run_bench(const std::vector<std::string>& args, std::string& out_json)
{
    if (args.empty())
        return emit_error(out_json,
                          "usage: context bench <attach|edit|bulk|query|sustained|import|merge> "
                          "--corpus <dir> [scenario flags]");
    const std::string& scenario = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    // Honest unsupported scenarios (the harness skips them): importers land with the M2 asset
    // pipeline; `context merge-file` (R-FILE-012) is an M2 verb — the M0 parse-bench spike remains
    // the file-format merge baseline until then.
    if (scenario == "import" || scenario == "merge")
    {
        Json out = Json::object();
        out.set("unsupported", Json(true));
        out_json = out.dump(2);
        return 0;
    }

    const std::optional<std::string> corpus_flag = flag_value(rest, "corpus");
    if (!corpus_flag.has_value())
        return emit_error(out_json, "bench " + scenario + " requires --corpus <dir>");
    std::error_code ec;
    const fs::path corpus = fs::absolute(fs::path(*corpus_flag), ec);
    if (ec)
        return emit_error(out_json, "could not resolve --corpus: " + ec.message());
    if (const std::optional<std::string> bad = check_corpus(corpus))
        return emit_error(out_json, *bad);

    if (scenario == "attach")
        return bench_attach(rest, corpus, out_json);
    if (scenario == "edit")
        return bench_edit(rest, corpus, out_json);
    if (scenario == "bulk")
        return bench_bulk(rest, corpus, out_json);
    if (scenario == "query")
        return bench_query(rest, corpus, out_json);
    if (scenario == "sustained")
        return bench_sustained(rest, corpus, out_json);
    return emit_error(out_json, "unknown bench scenario: '" + scenario + "'");
}

} // namespace context::cli
