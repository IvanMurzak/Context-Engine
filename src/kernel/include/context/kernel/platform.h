// Platform seam: the thin, injectable abstraction that keeps the kernel GPU-less and headless
// (R-HEAD-001) and gives the deterministic fault-injection harness its hooks (R-QA-010).
//
// Clock, FileSystem, and the threading entry point (TaskRunner) are pure interfaces. Nothing here
// touches a GPU or a display — the kernel runs identically on a headless VPS. Because every seam is
// virtual and injected, the M1 fault-injection harness (R-QA-010) can drive a virtual clock and a
// virtual filesystem deterministically (crash points, watcher loss, clock skew) without a real OS
// underneath. Concrete impls are intentionally minimal — the real native/watcher-backed impls land
// with the file-sync layer.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace context::kernel
{

// Monotonic clock seam. now_nanos() is a nondecreasing nanosecond counter (no wall-clock meaning).
class Clock
{
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual std::uint64_t now_nanos() const = 0;
};

// Minimal filesystem seam — enough for the kernel's headless needs and for the fault-injection
// harness to model missing/torn/duplicated reads. The real atomic-IO + watcher impl is the
// file-sync layer's (R-FILE-002/004), built ON these seams.
class FileSystem
{
public:
    virtual ~FileSystem() = default;
    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;
    [[nodiscard]] virtual std::optional<std::string> read(std::string_view path) const = 0;
    virtual bool write(std::string_view path, std::string_view data) = 0;
    virtual bool remove(std::string_view path) = 0;
};

// Threading entry point seam. The default is synchronous (InlineTaskRunner); a real pool arrives
// with the auto-parallel scheduler (R-SIM-006), behind this same interface.
class TaskRunner
{
public:
    virtual ~TaskRunner() = default;
    virtual void run(std::function<void()> task) = 0;
};

// The injectable platform bundle passed to the kernel. Non-owning: the owner keeps the concrete
// impls alive (see DefaultPlatform / kernel.h).
struct Platform
{
    Clock* clock = nullptr;
    FileSystem* fs = nullptr;
    TaskRunner* tasks = nullptr;
};

// --- concrete minimal implementations ---------------------------------------------------------

// Real monotonic clock backed by std::chrono::steady_clock.
class SteadyClock final : public Clock
{
public:
    [[nodiscard]] std::uint64_t now_nanos() const override;
};

// Test/fault-injection clock: time only moves when the harness moves it.
class ManualClock final : public Clock
{
public:
    explicit ManualClock(std::uint64_t start_nanos = 0) noexcept : now_(start_nanos) {}
    [[nodiscard]] std::uint64_t now_nanos() const override { return now_; }
    void advance(std::uint64_t delta_nanos) noexcept { now_ += delta_nanos; }
    void set(std::uint64_t nanos) noexcept { now_ = nanos; }

private:
    std::uint64_t now_;
};

// In-memory filesystem: deterministic, dependency-free, ideal for headless runs and fault injection.
class MemoryFileSystem final : public FileSystem
{
public:
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] std::optional<std::string> read(std::string_view path) const override;
    bool write(std::string_view path, std::string_view data) override;
    bool remove(std::string_view path) override;
    [[nodiscard]] std::size_t file_count() const noexcept { return files_.size(); }

private:
    std::unordered_map<std::string, std::string> files_;
};

// Synchronous task runner: runs the task inline on the calling thread.
class InlineTaskRunner final : public TaskRunner
{
public:
    void run(std::function<void()> task) override
    {
        if (task)
            task();
    }
};

} // namespace context::kernel
