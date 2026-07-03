// The file-sync layer's filesystem seam + an in-memory, fault-injectable implementation.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace context::editor::filesync
{

// Cheap metadata used by the cold-scan mtime+size gate (R-FILE-002). mtime is a monotonic nanosecond
// stamp with no wall-clock meaning — the gate only ever compares it for equality against the index.
struct FileStat
{
    std::uint64_t size = 0;
    std::uint64_t mtime_nanos = 0;
};

// Thrown by MemoryFileStore at an injected crash point — models the process dying mid-write, so a
// deterministic test can assert that no torn / partially-visible state is ever observed (R-QA-010).
// Real (native) FileStore impls never throw this.
struct SimulatedCrash : std::runtime_error
{
    explicit SimulatedCrash(const std::string& where)
        : std::runtime_error("simulated crash at " + where)
    {
    }
};

// The file-sync filesystem seam — richer than the kernel's minimal FileSystem (which has no atomic
// rename, stat, directory listing, or durability barrier). The kernel platform.h doc-comment
// anticipates exactly this layer: "the real atomic-IO + watcher impl is the file-sync layer's, built
// ON these seams". Every operation is virtual so the fault-injection harness can drive
// missing/torn/duplicated reads and crash points deterministically.
class FileStore
{
public:
    virtual ~FileStore() = default;

    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;
    [[nodiscard]] virtual std::optional<std::string> read(std::string_view path) const = 0;
    [[nodiscard]] virtual std::optional<FileStat> stat(std::string_view path) const = 0;

    // Recursively enumerate regular files at or beneath `dir`, normalized and sorted for
    // deterministic iteration (the full re-hash crawl walks this).
    [[nodiscard]] virtual std::vector<std::string> list(std::string_view dir) const = 0;

    virtual bool write(std::string_view path, std::string_view data) = 0;
    virtual bool rename(std::string_view from, std::string_view to) = 0; // atomic replace
    virtual bool remove(std::string_view path) = 0;
    virtual void fsync(std::string_view path) = 0; // durability barrier
};

// Deterministic, dependency-free in-memory FileStore with fault injection — the workhorse the M1
// fault-injection harness (R-QA-010) drives. Writes stamp a monotonically increasing synthetic mtime;
// tests can pin an mtime with set_mtime() to model a same-second in-place edit (mtime+size unchanged,
// content changed) that only a full re-hash catches.
class MemoryFileStore final : public FileStore
{
public:
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] std::optional<std::string> read(std::string_view path) const override;
    [[nodiscard]] std::optional<FileStat> stat(std::string_view path) const override;
    [[nodiscard]] std::vector<std::string> list(std::string_view dir) const override;
    bool write(std::string_view path, std::string_view data) override;
    bool rename(std::string_view from, std::string_view to) override;
    bool remove(std::string_view path) override;
    void fsync(std::string_view /*path*/) override {}

    // --- fault injection / test helpers -------------------------------------------------------

    // Arm a one-shot crash: the NEXT rename whose destination equals `path` throws SimulatedCrash
    // BEFORE moving anything — modelling a crash between the temp write and the rename.
    void crash_on_rename_to(std::string path);
    // Arm a one-shot crash on the next write to `path`.
    void crash_on_write(std::string path);
    // Pin an existing file's synthetic mtime (models same-second edits).
    void set_mtime(std::string_view path, std::uint64_t mtime_nanos);

    [[nodiscard]] std::size_t file_count() const noexcept { return files_.size(); }

private:
    struct Node
    {
        std::string data;
        std::uint64_t mtime = 0;
    };

    std::unordered_map<std::string, Node> files_;
    std::uint64_t next_mtime_ = 1;
    std::optional<std::string> crash_rename_dest_;
    std::optional<std::string> crash_write_path_;
};

} // namespace context::editor::filesync
