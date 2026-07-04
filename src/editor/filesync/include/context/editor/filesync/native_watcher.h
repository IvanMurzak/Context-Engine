// The native OS watcher backends behind the Watcher seam (R-FILE-002) — hints only, never truth.

#pragma once

#include "context/editor/filesync/watcher.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::filesync
{

// Real OS file-watcher backends behind the same `Watcher` seam `NullWatcher` / `FakeWatcher`
// implement — the design-deferred perf optimization of R-FILE-002. One class, three backends:
//
//   * Windows  — ReadDirectoryChangesW (native recursive subtree watch, overlapped IO).
//   * Linux    — inotify (per-directory watches added recursively; new subdirectories are watched
//                as they appear, and their pre-existing contents hinted to close the add race).
//   * macOS    — FSEvents (kFSEventStreamCreateFlagFileEvents on a private dispatch queue).
//
// The correctness model is UNCHANGED (L-22): every event this class delivers is a HINT that points
// at a path to re-hash — the content hash stays the truth, and the reconciler's low-frequency full
// re-hash crawl stays the dropped-event safety net. Hint loss is therefore always safe; it only
// costs latency, never correctness.
//
// degraded() semantics (R-FILE-002 — a silent fall-back to crawl latency is FORBIDDEN): the flag
// LATCHES true on the first loss-of-coverage signal and the reconciler then emits the visible
// `watcher.degraded` diagnostic. Latching signals, per backend:
//   * registration failure at construction (missing/non-directory root, network FS refusing the
//     watch, per-user inotify instance/watch limits — ENOSPC/EMFILE, CreateFileW failure);
//   * kernel-side event loss (RDCW buffer overflow / zero-byte completion, inotify IN_Q_OVERFLOW,
//     FSEvents MustScanSubDirs/KernelDropped/UserDropped coalescing);
//   * backend death (watch re-issue failure, watch-thread error) and the userspace pending-queue
//     cap overflowing under an event storm.
// A degraded watcher KEEPS delivering whatever hints it still can (they still cut latency); the
// latch is about honest visibility, not about stopping.
//
// Threading: each backend collects events on ONE private thread (Windows/Linux) or dispatch queue
// (macOS) into a mutex-guarded pending queue; poll() drains it. poll()/degraded() are safe to call
// from any thread; the instance itself is single-owner like every other Watcher. The destructor
// stops and joins the backend. Construction never throws for environmental reasons — a watcher
// that cannot register reports degraded() == true and delivers nothing, exactly like NullWatcher.
//
// Path mapping: the watcher watches `watch_dir` on the REAL filesystem and emits LOGICAL seam paths
// — `<logical_prefix>/<path relative to watch_dir>`, normalized via normalize_path() — so its hints
// key straight into the same reconcile index a NativeFileStore-backed Reconciler maintains (the
// `context daemon` composition: NativeFileStore(base) + NativeWatcher(base/proj, "proj")).
class NativeWatcher final : public Watcher
{
public:
    // Watch the subtree rooted at `watch_dir` (must already exist and be a directory — otherwise
    // the watcher comes up degraded; it does NOT create directories). `logical_prefix` is prepended
    // to every emitted relative path ("" ⇒ paths are emitted relative to watch_dir directly).
    NativeWatcher(std::filesystem::path watch_dir, std::string logical_prefix);
    ~NativeWatcher() override;

    NativeWatcher(const NativeWatcher&) = delete;
    NativeWatcher& operator=(const NativeWatcher&) = delete;

    // Drain the pending hints collected since the last poll (deduplicated by path — kind is
    // advisory, the reconciler re-hashes unconditionally either way).
    [[nodiscard]] std::vector<WatchEvent> poll() override;

    // Latched loss-of-coverage flag — see the class comment for the latching signals.
    [[nodiscard]] bool degraded() const override;

    // True when this build carries a real backend for the current platform (Windows / Linux /
    // macOS). On any other platform NativeWatcher is constructible but permanently degraded and
    // emits nothing — NullWatcher semantics, so composition code needs no #ifdef.
    [[nodiscard]] static bool backend_available() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace context::editor::filesync
