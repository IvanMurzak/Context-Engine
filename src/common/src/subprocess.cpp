// The shared subprocess runner (see context/common/subprocess.h). Extracted verbatim from the
// shadercc WGSL leg's hardened helpers (the strictest of the diverged copies) so migrating that
// backend to this module leaves its compile() output byte-identical (issue #146).

#include "context/common/subprocess.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <sys/wait.h> // WIFEXITED / WEXITSTATUS — decode std::system()'s POSIX wait status
#endif

namespace context::common::subprocess
{
namespace
{

// The per-shell metacharacter reject sets (the reconciled strictest policy, from the shadercc WGSL
// leg). Cmd: cmd.exe expands %VAR% even inside quotes, and a bare `"` ends the quoted token — both
// refused; `\` is the Windows path separator (legitimate), so it is NOT a metacharacter. Posix: inside
// "..." the shell still expands `$` and backtick and treats `\` specially, and `"` ends the token —
// all four refused.
constexpr std::string_view kCmdMeta = "\"%";
constexpr std::string_view kPosixMeta = "\"$`\\";

constexpr std::string_view meta_for(Shell shell) noexcept
{
    return shell == Shell::Cmd ? kCmdMeta : kPosixMeta;
}

} // namespace

bool has_shell_metacharacters(std::string_view arg, Shell shell) noexcept
{
    return arg.find_first_of(meta_for(shell)) != std::string_view::npos;
}

std::string quote_argument(std::string_view arg, Shell shell)
{
    if (has_shell_metacharacters(arg, shell))
    {
        throw MetacharacterError(
            "subprocess: refusing to quote argument with shell metacharacters: " + std::string(arg));
    }
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    out.append(arg.data(), arg.size());
    out.push_back('"');
    return out;
}

std::string quote_argument(std::string_view arg)
{
    return quote_argument(arg, host_shell());
}

std::string wrap_command(std::string_view command, Shell shell)
{
    if (shell == Shell::Cmd)
    {
        std::string out;
        out.reserve(command.size() + 2);
        out.push_back('"');
        out.append(command.data(), command.size());
        out.push_back('"');
        return out;
    }
    return std::string(command);
}

int run_command(std::string_view command)
{
    const std::string wrapped = wrap_command(command, host_shell());
    const int status = std::system(wrapped.c_str());
#ifdef _WIN32
    return status; // cmd.exe returns the child's exit code directly.
#else
    if (status == -1)
    {
        return -1; // the shell/command processor could not be launched at all.
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::filesystem::path make_scratch_path(std::string_view prefix, std::string_view suffix)
{
    static const std::uint64_t process_tag = [] {
        std::random_device rd;
        return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }();
    static std::atomic<std::uint64_t> counter{0};

    std::ostringstream name;
    name << prefix << '-' << std::hex << process_tag << '-' << std::dec
         << counter.fetch_add(1, std::memory_order_relaxed) << suffix;
    return std::filesystem::temp_directory_path() / name.str();
}

ScratchFile::ScratchFile(std::filesystem::path p) : path_(std::move(p)) {}

ScratchFile::~ScratchFile()
{
    std::error_code ec;
    std::filesystem::remove(path_, ec); // best-effort; never throws from a destructor.
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const std::filesystem::path& path, const void* data, std::size_t bytes)
{
    std::ofstream out(path, std::ios::binary);
    if (bytes != 0) // an empty write still creates the (empty) file for a tool to reject loudly
    {
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    }
    return static_cast<bool>(out);
}

} // namespace context::common::subprocess
