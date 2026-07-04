// Watch-hash-reconcile pipeline implementation.

#include "context/editor/filesync/reconciler.h"

#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/event_bus.h"
#include "context/kernel/platform.h"

#include <set>
#include <utility>
#include <vector>

namespace context::editor::filesync
{

Reconciler::Reconciler(FileStore& fs, Watcher& watcher, context::kernel::Clock& clock,
                       context::kernel::TaskRunner& tasks, std::string root, std::string index_path,
                       context::kernel::EventBus* bus, ReconcilerConfig config)
    : fs_(fs), watcher_(watcher), clock_(clock), tasks_(tasks), bus_(bus), root_(std::move(root)),
      index_path_(std::move(index_path)),
      crawl_fallback_note_(std::move(config.crawl_fallback_note)),
      expected_(config.expected_write_ttl_nanos)
{
}

// Control-path classification lives in atomic_io.h (is_control_path) — shared with the sidecar
// orphan sweep so the two never drift.

void Reconciler::announce_degraded_if_needed()
{
    if (!watcher_.degraded())
    {
        degraded_announced_ = false;
        return;
    }
    if (degraded_announced_)
        return;
    degraded_announced_ = true;
    if (bus_ != nullptr)
    {
        // R-FILE-002: the degraded diagnostic names the affected subtree AND the fallback cadence —
        // a silent fall-back to crawl latency is forbidden.
        bus_->log(context::kernel::LogLevel::warn,
                  "watcher.degraded: OS watch registration failed/truncated for subtree '" + root_ +
                      "'; change-detection falls back to " + crawl_fallback_note_);
    }
}

std::optional<ReconcileChange> Reconciler::rehash(std::string_view path, std::uint64_t now_nanos)
{
    const std::string key = normalize_path(path);
    const std::optional<std::string> current = fs_.read(key);

    if (!current)
    {
        // The file is gone. Report a removal only if we were tracking it.
        if (index_.get(key))
        {
            index_.erase(key);
            return ReconcileChange{key, ChangeType::removed, 0};
        }
        return std::nullopt;
    }

    const std::uint64_t hash = content_hash(*current);

    // Self-echo suppression: our own registered write is not an external change. Keep the index in
    // sync (it already should be) and report nothing.
    if (expected_.is_self_echo(key, hash, now_nanos))
    {
        if (const std::optional<FileStat> st = fs_.stat(key))
            index_.put(key, IndexEntry{st->size, st->mtime_nanos, hash});
        return std::nullopt;
    }

    const std::optional<IndexEntry> prev = index_.get(key);
    if (prev && prev->content_hash == hash)
    {
        // Content unchanged — the hash is the truth, the hint/mtime bump was spurious. Refresh the
        // index's cheap-gate stat (mtime+size) so a later gated crawl short-circuits instead of
        // re-reading + re-hashing this file forever (a bare `touch` bumps mtime without changing bytes).
        if (const std::optional<FileStat> st = fs_.stat(key))
            index_.put(key, IndexEntry{st->size, st->mtime_nanos, hash});
        return std::nullopt;
    }

    const std::optional<FileStat> st = fs_.stat(key);
    index_.put(key, IndexEntry{st ? st->size : static_cast<std::uint64_t>(current->size()),
                               st ? st->mtime_nanos : 0, hash});
    return ReconcileChange{key, prev ? ChangeType::modified : ChangeType::created, hash};
}

std::size_t Reconciler::attach()
{
    index_ = ReconcileIndex::load(fs_, index_path_);
    return index_.size();
}

bool Reconciler::apply_write(std::string_view path, std::string_view data)
{
    const std::string key = normalize_path(path);
    if (!is_inside_jail(root_, key))
        return false;

    const std::uint64_t now = clock_.now_nanos();
    if (!atomic_write(fs_, key, data, std::to_string(now)))
        return false;

    const std::uint64_t hash = content_hash(data);
    expected_.register_write(key, hash, now);
    if (const std::optional<FileStat> st = fs_.stat(key))
        index_.put(key, IndexEntry{st->size, st->mtime_nanos, hash});
    return true;
}

std::vector<ReconcileChange> Reconciler::reconcile_hints()
{
    const std::uint64_t now = clock_.now_nanos();
    expected_.expire(now);
    announce_degraded_if_needed();

    std::vector<ReconcileChange> changes;
    for (const WatchEvent& event : watcher_.poll())
    {
        if (is_control_path(event.path))
            continue;
        if (std::optional<ReconcileChange> change = rehash(event.path, now))
            changes.push_back(std::move(*change));
    }
    expand_sidecar_owners(changes);
    return changes;
}

std::vector<ReconcileChange> Reconciler::crawl(bool gated)
{
    const std::uint64_t now = clock_.now_nanos();
    expected_.expire(now);

    std::vector<ReconcileChange> changes;
    tasks_.run(
        [&]
        {
            std::set<std::string> seen;
            for (const std::string& path : fs_.list(root_))
            {
                if (is_control_path(path))
                    continue;
                seen.insert(path);

                if (gated)
                {
                    const std::optional<FileStat> st = fs_.stat(path);
                    const std::optional<IndexEntry> prev = index_.get(path);
                    if (prev && st && prev->size == st->size && prev->mtime_nanos == st->mtime_nanos)
                        continue; // cheap gate: nothing changed by mtime+size — skip the read.
                }

                if (std::optional<ReconcileChange> change = rehash(path, now))
                    changes.push_back(std::move(*change));
            }

            // Deletions: index rows whose file no longer exists on disk.
            std::vector<std::string> removed;
            for (const auto& [path, entry] : index_.entries())
            {
                (void)entry;
                if (is_control_path(path))
                    continue;
                if (seen.find(path) == seen.end())
                    removed.push_back(path);
            }
            for (const std::string& path : removed)
            {
                index_.erase(path);
                changes.push_back(ReconcileChange{path, ChangeType::removed, 0});
            }
        });
    expand_sidecar_owners(changes);
    return changes;
}

bool Reconciler::save_index()
{
    return index_.save(fs_, index_path_);
}

void Reconciler::set_sidecar_refs(std::string_view owner, std::vector<std::string> sidecar_paths)
{
    sidecars_.set_owner_refs(owner, std::move(sidecar_paths));
}

void Reconciler::clear_sidecar_refs(std::string_view owner)
{
    sidecars_.remove_owner(owner);
}

void Reconciler::expand_sidecar_owners(std::vector<ReconcileChange>& changes)
{
    if (sidecars_.owner_count() == 0 || changes.empty())
        return;

    std::set<std::string> present;
    for (const ReconcileChange& change : changes)
        present.insert(change.path);

    // Index-based over the DIRECT changes only (appends must not re-expand; push_back may
    // reallocate, so copy the fields we need before appending).
    const std::size_t direct_count = changes.size();
    for (std::size_t i = 0; i < direct_count; ++i)
    {
        const std::string path = changes[i].path;
        const ChangeType type = changes[i].type;

        // A reconciled owner removal drops its registrations — a gone owner dirties nothing.
        if (type == ChangeType::removed && sidecars_.has_owner(path))
            sidecars_.remove_owner(path);

        // A changed (or removed) sidecar dirties every registered owner whose bytes did not also
        // change this pass (an owner that changed directly is already in the list). L-33: the
        // owner's derived output embeds the sidecar bytes, so it must re-derive; a removal makes
        // the owner's ref dangling, which the re-derive then diagnoses (R-FILE-003).
        for (const std::string& owner : sidecars_.owners_of(path))
        {
            if (present.count(owner) != 0)
                continue;
            const std::optional<IndexEntry> entry = index_.get(owner);
            if (!entry)
                continue; // the owner is not tracked (never scanned or already gone) — nothing to dirty
            present.insert(owner);
            changes.push_back(
                ReconcileChange{owner, ChangeType::modified, entry->content_hash, path});
        }
    }
}

} // namespace context::editor::filesync
