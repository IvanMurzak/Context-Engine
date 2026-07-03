// The watcher seam — OS file watchers treated as HINTS, never as the source of truth (R-FILE-002).

#pragma once

#include <string>
#include <vector>

namespace context::editor::filesync
{

enum class ChangeKind
{
    created,
    modified,
    removed,
};

// A single watcher hint. It only POINTS AT a path to re-hash; the content hash decides whether
// anything actually changed. `kind` is advisory (watchers routinely misreport it).
struct WatchEvent
{
    std::string path;
    ChangeKind kind = ChangeKind::modified;
};

// The watcher seam. poll() drains the pending hints. degraded() is true when OS watch registration
// failed or truncated (per-user inotify/fd limits exhausted, network filesystems, watch-hostile
// mounts): the reconciler then leans entirely on the crawl AND must emit a visible watcher.degraded
// diagnostic — a silent fall-back to crawl latency is forbidden (R-FILE-002).
class Watcher
{
public:
    virtual ~Watcher() = default;
    [[nodiscard]] virtual std::vector<WatchEvent> poll() = 0;
    [[nodiscard]] virtual bool degraded() const = 0;
};

// Portable default: registers no OS watch and is always degraded, so correctness comes ENTIRELY from
// the reconcile crawl. A real inotify / ReadDirectoryChangesW / FSEvents impl is a later,
// platform-specific drop-in behind this same seam; because hashes are authoritative, correctness
// never depends on the watcher being present or accurate.
class NullWatcher final : public Watcher
{
public:
    [[nodiscard]] std::vector<WatchEvent> poll() override { return {}; }
    [[nodiscard]] bool degraded() const override { return true; }
};

// Test / fault-injection watcher: the harness enqueues hints and can drop, duplicate, and reorder
// them (R-QA-010 asserts convergence under exactly these faults). poll() drains and clears.
class FakeWatcher final : public Watcher
{
public:
    void emit(WatchEvent event);
    void emit(std::string path, ChangeKind kind);
    void drop_all();      // model losing every pending event
    void duplicate_all(); // model duplicated delivery
    void reverse();       // model reordered delivery
    void set_degraded(bool degraded) { degraded_ = degraded; }

    [[nodiscard]] std::vector<WatchEvent> poll() override;
    [[nodiscard]] bool degraded() const override { return degraded_; }

private:
    std::vector<WatchEvent> pending_;
    bool degraded_ = false;
};

} // namespace context::editor::filesync
