// One hardened home for the repo's `std::system` subprocess + scratch-temp-file runner (issue #146,
// post-PR-#134): argv-style quoting with a single fail-closed metacharacter policy, the cmd.exe
// outer-quote workaround, exit-code mapping, and scratch-file RAII. Consolidates the copies that had
// diverged across the shadercc WGSL leg (the hardened reference), the esbuild + tsgo TS drivers, and
// the real-git merge-driver test. The metacharacter policy is the STRICTEST of those copies (shadercc).

#pragma once

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace context::common::subprocess
{

// Which shell std::system() hands the command to. The metacharacter policy AND the outer-quote
// workaround differ per shell, so callers/tests name the target shell explicitly — which makes the
// policy testable on BOTH shapes from ANY host (a POSIX branch is otherwise not even compiled on the
// Windows dev gate, so a #ifdef-only policy could never be unit-tested cross-platform).
enum class Shell
{
    Posix, // /bin/sh: inside "..." the shell still expands $ and `...`, and treats \ specially
    Cmd,   // cmd.exe: %VAR% expands even inside "..."; \ is the path separator (legitimate)
};

// The shell std::system() uses on the CURRENT host.
[[nodiscard]] constexpr Shell host_shell() noexcept
{
#ifdef _WIN32
    return Shell::Cmd;
#else
    return Shell::Posix;
#endif
}

// Raised by quote_argument() when the argument carries a shell metacharacter. Fail-closed: refuse to
// build an ambiguous / injectable command line rather than quote around a metacharacter. Scratch
// paths and CMake-baked tool paths never legitimately contain these, so a throw means a real defect.
class MetacharacterError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// True when `arg` contains a metacharacter `shell` would expand inside a double-quoted token. The
// single reconciled policy (the strictest — the shadercc WGSL leg's): Cmd rejects `"` and `%`; Posix
// rejects `"`, `$`, backtick, and `\`. A space is NOT a metacharacter — that is what quoting is for.
[[nodiscard]] bool has_shell_metacharacters(std::string_view arg, Shell shell) noexcept;

// Double-quote one command-line argument for `shell`, fail-closed: throws MetacharacterError when
// has_shell_metacharacters(arg, shell). Returns "\"" + arg + "\"" otherwise.
[[nodiscard]] std::string quote_argument(std::string_view arg, Shell shell);
[[nodiscard]] std::string quote_argument(std::string_view arg); // == quote_argument(arg, host_shell())

// Wrap a fully-assembled command line for `shell` so std::system() runs it intact. cmd.exe strips the
// OUTERMOST quote pair from `cmd /c <line>`, mangling a line with several quoted segments (`"exe" …
// "path"`), so the whole line is wrapped in one extra pair; POSIX passes through unchanged. Pure (it
// launches nothing), so both shapes are unit-testable from any host.
[[nodiscard]] std::string wrap_command(std::string_view command, Shell shell);

// Run a fully-assembled command line via std::system(), applying wrap_command() for the host shell.
// Returns the process exit code, or -1 when the shell/process could not be launched at all (POSIX
// decodes the wait status via WIFEXITED/WEXITSTATUS; Windows returns the child's code directly).
[[nodiscard]] int run_command(std::string_view command);

// A unique scratch path under the system temp dir: `<prefix>-<per-process-tag>-<counter><suffix>`.
// The random per-process tag + an atomic counter keep concurrent processes (ctest -j) and repeated
// calls from colliding. Scratch paths are I/O plumbing only — the name never affects a caller's output.
[[nodiscard]] std::filesystem::path make_scratch_path(std::string_view prefix, std::string_view suffix);

// A scratch file that removes itself (best-effort) on scope exit, so no scratch accumulates across
// corpus-sized runs. Never throws from its destructor.
class ScratchFile
{
public:
    explicit ScratchFile(std::filesystem::path p);
    ~ScratchFile();
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Read a whole file into a string ("" when absent/unreadable — a missing tool stdout on a failed run
// is expected, not an error to re-report). Binary-safe.
[[nodiscard]] std::string read_file(const std::filesystem::path& path);

// Write `bytes` to `path` (binary, truncating). Returns false on any IO failure. An empty write still
// creates the (empty) file so a downstream tool can reject it loudly rather than see a missing input.
[[nodiscard]] bool write_file(const std::filesystem::path& path, const void* data, std::size_t bytes);

} // namespace context::common::subprocess
