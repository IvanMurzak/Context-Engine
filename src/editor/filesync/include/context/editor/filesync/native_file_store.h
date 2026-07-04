// The native (on-disk) FileStore: the real atomic-IO backend behind the file-sync filesystem seam.

#pragma once

#include "context/editor/filesync/file_store.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::filesync
{

// A concrete FileStore over std::filesystem + platform durability primitives (R-FILE-002/004). It
// backs the file-authoritative loop on an ACTUAL filesystem — the counterpart of MemoryFileStore,
// which stays selectable for deterministic, fault-injectable tests. The header contract holds: a
// native impl never throws SimulatedCrash; crash points are modelled in tests by a decorator, not by
// the store itself.
//
// Logical <-> real mapping: every seam path is a LOGICAL string (e.g. "proj/a.scene"), normalized the
// same way MemoryFileStore keys and the reconcile index are, and resolved under `base` on real disk
// (`base / normalize_path(path)`). list() returns logical paths relative to `base`, so its output feeds
// straight back into read()/stat() and matches the index keys the reconciler stores.
//
// Atomicity + durability (R-FILE-004): rename() is an atomic same-volume replace (POSIX renameat(2)
// relative to verified directory fds; Windows SetFileInformationByHandle + FILE_RENAME_INFO with
// ReplaceIfExists, handle-relative to the verified destination directory — native primitives are
// used directly because MinGW's std::filesystem::rename maps to _wrename, which does NOT replace an
// existing destination). fsync() is a real durability barrier (POSIX fsync on the file +
// best-effort parent-dir fsync so the rename is durable; Windows FlushFileBuffers). Bytes are
// read/written in BINARY mode so content hashes are byte-exact and identical across platforms (no
// CRLF translation).
//
// Path jail: callers (reconciler / WriteQueue) apply the LOGICAL jail (normalize + is_inside_jail);
// this store additionally enforces the R-SEC-008 TOCTOU-safe PHYSICAL jail on every write-side
// operation (write/rename/remove) — resolve-then-verify-by-fd/handle: POSIX opens/acts relative to
// a pinned, verified parent-directory fd with O_NOFOLLOW on the leaf and re-realpaths the opened fd
// (/proc/self/fd, F_GETPATH); Windows opens the target with FILE_FLAG_OPEN_REPARSE_POINT (a reparse
// leaf is refused) and verifies GetFinalPathNameByHandleW on the OPENED handle before any byte
// lands. Root-escaping symlinks/junctions are refused; internal (non-escaping) links are allowed;
// an unresolvable jail root fails CLOSED. Because the intent-log resume (WriteQueue::recover) and
// every atomic_write route through this same seam, crash-recovery writes get the identical
// verification (R-FILE-004 "jail on resume"). Read-side ops keep the lexical mapping — a
// documented follow-up; they never land or redirect content.
class NativeFileStore final : public FileStore
{
public:
    // `base` is the real on-disk directory the logical FileStore root maps to (typically the daemon's
    // project_root). It is not required to exist yet — write()/rename() create parent directories on
    // demand, mirroring MemoryFileStore's create-on-insert behavior.
    explicit NativeFileStore(std::filesystem::path base);

    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] std::optional<std::string> read(std::string_view path) const override;
    [[nodiscard]] std::optional<FileStat> stat(std::string_view path) const override;
    [[nodiscard]] std::vector<std::string> list(std::string_view dir) const override;
    bool write(std::string_view path, std::string_view data) override;
    bool rename(std::string_view from, std::string_view to) override; // atomic replace
    bool remove(std::string_view path) override;
    void fsync(std::string_view path) override; // durability barrier

    [[nodiscard]] const std::filesystem::path& base() const noexcept { return base_; }

private:
    // Resolve a logical seam path to its real on-disk path: `base_ / normalize_path(logical)`.
    [[nodiscard]] std::filesystem::path resolve(std::string_view logical) const;

    // The RESOLVED (symlink-free, comparison-folded) jail root every write-side operation verifies
    // its opened fd/handle against (R-SEC-008). Resolved lazily on first use (base_ may not exist
    // at construction; write() creates it). EMPTY when resolution fails — the jail then fails
    // CLOSED (write-side ops refuse) rather than degrading to the lexical check a symlink swap
    // defeats.
    [[nodiscard]] const std::filesystem::path& real_base() const;

    std::filesystem::path base_;
    mutable std::filesystem::path real_base_;
};

} // namespace context::editor::filesync
