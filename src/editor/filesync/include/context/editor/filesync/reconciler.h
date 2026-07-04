// The watch-hash-reconcile pipeline (R-FILE-002) — content hashing is authoritative over watchers.

#pragma once

#include "context/editor/filesync/expected_writes.h"
#include "context/editor/filesync/reconcile_index.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::kernel
{
class Clock;
class TaskRunner;
class EventBus;
} // namespace context::kernel

namespace context::editor::filesync
{

class FileStore;
class Watcher;

enum class ChangeType
{
    created,
    modified,
    removed,
};

// A reconciled external change the pipeline surfaces to its consumer (the derivation graph, later).
struct ReconcileChange
{
    std::string path;
    ChangeType type = ChangeType::modified;
    std::uint64_t content_hash = 0; // 0 for a removal
};

struct ReconcilerConfig
{
    // Self-echo suppression window (~one debounce cycle). Short by design: it must never outlive a
    // genuine later external edit of the same path (R-FILE-002).
    std::uint64_t expected_write_ttl_nanos = 500000000ULL; // 500 ms

    // Names the crawl fallback + its cadence inside the `watcher.degraded` diagnostic — R-FILE-002
    // requires the diagnostic to name the affected subtree AND the fallback cadence. The default is
    // accurate for a crawl-every-pass composition (EditorKernelConfig.min_crawl_interval_nanos == 0,
    // the no-real-watcher default); EditorKernel sets a cadence-accurate wording when its
    // low-frequency crawl policy is active (interval > 0), and any other timer-driven consumer sets
    // its own.
    std::string crawl_fallback_note = "the full re-hash crawl (runs with every reconcile pass)";
};

// The watch-hash-reconcile pipeline. Watcher events are HINTS only: they point at paths to re-hash,
// and the content hash decides truth. The low-frequency full re-hash crawl is the dropped-event
// safety net — convergence is guaranteed even if EVERY watcher event for a path is lost. Consumes the
// kernel platform seams read-only: Clock (TTL / timestamps), TaskRunner (runs the crawl body),
// EventBus (emits the watcher.degraded log diagnostic). Fully deterministic under injected fakes
// (R-QA-010).
class Reconciler
{
public:
    Reconciler(FileStore& fs, Watcher& watcher, context::kernel::Clock& clock,
               context::kernel::TaskRunner& tasks, std::string root, std::string index_path,
               context::kernel::EventBus* bus = nullptr, ReconcilerConfig config = {});

    // Warm attach: load the persisted index (mtime/size-gated) — no cold full re-hash when the index
    // is valid. Returns the number of entries loaded.
    std::size_t attach();

    // Daemon-initiated write: register the expected write (self-echo suppression) then atomically
    // write, folding the resulting state straight into the index (our own write is not an external
    // change). Path-jailed. Returns durable success.
    bool apply_write(std::string_view path, std::string_view data);

    // Drain watcher hints; each hinted path is re-hashed UNCONDITIONALLY (mtime granularity is
    // untrusted for content equality), self-echo-suppressed, and reconciled against the index.
    // Duplicated / reordered hints are harmless (a second re-hash of an already-current file yields no
    // change). Emits watcher.degraded once per degraded transition.
    std::vector<ReconcileChange> reconcile_hints();

    // The dropped-event safety net. gated=true: the cheap warm cold-scan — a path whose mtime AND size
    // match the index is skipped without reading. gated=false: the low-frequency FULL re-hash crawl
    // that guarantees eventual convergence even for a same-mtime+size in-place edit whose every
    // watcher event was lost. Also detects deletions (index rows with no file). Runs its body through
    // the injected TaskRunner.
    std::vector<ReconcileChange> crawl(bool gated = true);

    bool save_index();
    [[nodiscard]] const ReconcileIndex& index() const noexcept { return index_; }

private:
    // Re-read + re-hash `path`, reconcile against the index, and return the external change (if any).
    // Applies self-echo suppression. A removal is reported when the file is gone but indexed.
    std::optional<ReconcileChange> rehash(std::string_view path, std::uint64_t now_nanos);
    [[nodiscard]] bool is_control_path(std::string_view path) const;
    void announce_degraded_if_needed();

    FileStore& fs_;
    Watcher& watcher_;
    context::kernel::Clock& clock_;
    context::kernel::TaskRunner& tasks_;
    context::kernel::EventBus* bus_;
    std::string root_;
    std::string index_path_;
    std::string crawl_fallback_note_;
    ReconcileIndex index_;
    ExpectedWrites expected_;
    bool degraded_announced_ = false;
};

} // namespace context::editor::filesync
