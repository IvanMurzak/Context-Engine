// Test-only support for the native (on-disk) FileStore ctests: a RAII temp directory on the REAL
// filesystem, and a crash-injecting FileStore decorator so a deterministic "process died mid-write"
// crash can be modelled over real IO. Neither belongs in the shipped library — a production
// NativeFileStore never throws SimulatedCrash (file_store.h contract); the decorator provides that
// deterministic crash point ONLY for tests, forwarding every other op to the real store underneath.

#pragma once

#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/path_jail.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace nfstest
{

// A unique temp directory on the real filesystem, removed (best-effort) on destruction.
class TempDir
{
public:
    explicit TempDir(const char* tag)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ctx-native-" + std::string(tag) + "-" + std::to_string(stamp));
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Forwards every FileStore op to a real (native) store, but throws SimulatedCrash on the next rename
// whose destination matches an armed path — modelling the process dying between the durable temp write
// and the atomic rename, over real IO. One-shot: the arm clears when it fires.
class CrashInjectingFileStore final : public context::editor::filesync::FileStore
{
public:
    explicit CrashInjectingFileStore(context::editor::filesync::FileStore& inner) : inner_(inner) {}

    // Arm a one-shot crash on the next rename whose destination equals `dst` (normalized like the store).
    void crash_on_rename_to(std::string dst)
    {
        crash_dst_ = context::editor::filesync::normalize_path(dst);
    }

    [[nodiscard]] bool exists(std::string_view path) const override { return inner_.exists(path); }
    [[nodiscard]] std::optional<std::string> read(std::string_view path) const override
    {
        return inner_.read(path);
    }
    [[nodiscard]] std::optional<context::editor::filesync::FileStat>
    stat(std::string_view path) const override
    {
        return inner_.stat(path);
    }
    [[nodiscard]] std::vector<std::string> list(std::string_view dir) const override
    {
        return inner_.list(dir);
    }
    bool write(std::string_view path, std::string_view data) override
    {
        return inner_.write(path, data);
    }
    bool rename(std::string_view from, std::string_view to) override
    {
        if (crash_dst_ && *crash_dst_ == context::editor::filesync::normalize_path(to))
        {
            crash_dst_.reset();
            throw context::editor::filesync::SimulatedCrash("rename to " + std::string(to));
        }
        return inner_.rename(from, to);
    }
    bool remove(std::string_view path) override { return inner_.remove(path); }
    void fsync(std::string_view path) override { inner_.fsync(path); }

private:
    context::editor::filesync::FileStore& inner_;
    std::optional<std::string> crash_dst_;
};

// Create a DIRECTORY link at `link` pointing at `target` (an existing real directory), for the
// R-SEC-008 symlink-escape fault cases. POSIX: a plain directory symlink (always available).
// Windows: try a directory symlink first (works under Developer Mode / an elevated CI runner), then
// fall back to an NTFS JUNCTION via `cmd /c mklink /J` (junctions need no privilege — and are a
// reparse point the TOCTOU-safe jail must treat exactly like a symlink). Returns false when no link
// could be created — the caller SKIPS the dependent fault case (the attack precondition cannot be
// staged on this host) rather than failing.
inline bool make_dir_link(const std::filesystem::path& link, const std::filesystem::path& target)
{
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link, ec);
    if (!ec && std::filesystem::is_symlink(link))
        return true;
#if defined(_WIN32)
    std::filesystem::path l = link;
    std::filesystem::path t = target;
    const std::string cmd = "cmd /c mklink /J \"" + l.make_preferred().string() + "\" \"" +
                            t.make_preferred().string() + "\" > NUL 2>&1";
    (void)std::system(cmd.c_str());
    std::error_code jec;
    return std::filesystem::exists(link, jec); // the junction resolves to the existing target
#else
    return false;
#endif
}

// Create a FILE symlink at `link` -> `target`. May be unavailable on Windows without Developer
// Mode / elevation (and junctions are directory-only, so there is no fallback); the caller skips
// the dependent fault case when this returns false.
inline bool make_file_link(const std::filesystem::path& link, const std::filesystem::path& target)
{
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    return !ec && std::filesystem::is_symlink(link);
}

} // namespace nfstest
