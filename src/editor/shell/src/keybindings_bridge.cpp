// The keybindings read/watch bridge surface (M9 e07c) — see keybindings_bridge.h for the D10
// boundary-clean rationale, the bytes-only (schema lives in editor-core) split, and the generation
// watch model.

#include "context/editor/shell/keybindings_bridge.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

namespace context::editor::shell
{
namespace
{

namespace fs = std::filesystem;
using contract::Json;

// Read an environment variable's VALUE, or nullopt when unset/empty. MSVC rejects std::getenv as
// C4996 under /W4 /WX, so the _MSC_VER branch uses the two-call getenv_s (first sizes the buffer, then
// fills it); the local Strawberry-GCC gate + the Clang CI legs compile the std::getenv branch. This is
// the value-reading sibling of doctor_command.cpp's presence-only env_present().
[[nodiscard]] std::optional<std::string> read_env(const char* name)
{
#if defined(_MSC_VER)
    std::size_t required = 0;
    if (::getenv_s(&required, nullptr, 0, name) != 0 || required == 0)
    {
        return std::nullopt;
    }
    std::string value(required, '\0');
    if (::getenv_s(&required, value.data(), value.size(), name) != 0)
    {
        return std::nullopt;
    }
    // getenv_s writes the C string plus its terminating null into the buffer; trim to the real length.
    value.resize(std::strlen(value.c_str()));
    if (value.empty())
    {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

// Read a small file into `out` (binary, so bytes round-trip). Returns false on any IO error OR when
// the file exceeds the cap — an oversized override is treated as unreadable rather than shipped, which
// the caller surfaces as an ABSENT snapshot (editor-core then stays on the defaults). `size` is the
// already-stat'd file size, passed in so this does not re-stat.
[[nodiscard]] bool read_small_file(const fs::path& path, std::uintmax_t size, std::string& out)
{
    if (size > kMaxKeybindingsBytes)
    {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    // A read error mid-stream (e.g. a file that vanished between stat and read) fails closed.
    if (stream.bad())
    {
        return false;
    }
    out = buffer.str();
    return true;
}

} // namespace

std::optional<fs::path> home_directory()
{
#if defined(_WIN32)
    // %USERPROFILE% is the standard Windows home. (HOMEDRIVE+HOMEPATH is an older fallback, not needed
    // for the modern targets this ships on.)
    if (const std::optional<std::string> profile = read_env("USERPROFILE"))
    {
        return fs::path(*profile);
    }
#else
    if (const std::optional<std::string> home = read_env("HOME"))
    {
        return fs::path(*home);
    }
#endif
    return std::nullopt;
}

fs::path default_keybindings_path()
{
    const std::optional<fs::path> home = home_directory();
    if (!home.has_value())
    {
        return {}; // no home -> an empty path -> a permanently-absent snapshot (defaults stand)
    }
    return *home / ".context" / "keybindings.json";
}

// -------------------------------------------------------------------------------- the bridge

bool KeybindingsBridge::adopt(bool present, std::string text)
{
    if (present == snapshot_.present && text == snapshot_.text)
    {
        return false; // no observed change -> no generation bump (a byte-identical rewrite is a no-op)
    }
    snapshot_.present = present;
    snapshot_.text = std::move(text);
    ++snapshot_.generation;
    return true;
}

bool KeybindingsBridge::refresh_from_disk()
{
    std::error_code ec;
    if (path_.empty())
    {
        have_stat_ = false;
        return adopt(false, std::string());
    }
    const bool is_file = fs::is_regular_file(path_, ec) && !ec;
    if (!is_file)
    {
        // Absent (or a directory / special file). Reset the stat cache so a later re-creation re-reads.
        have_stat_ = false;
        return adopt(false, std::string());
    }
    const std::uintmax_t size = fs::file_size(path_, ec);
    if (ec)
    {
        have_stat_ = false;
        return adopt(false, std::string());
    }
    std::string content;
    if (!read_small_file(path_, size, content))
    {
        // Unreadable or oversized -> absent. Do NOT cache the stat, so a shrink/permission-fix re-reads.
        have_stat_ = false;
        return adopt(false, std::string());
    }
    const fs::file_time_type mtime = fs::last_write_time(path_, ec);
    if (!ec)
    {
        have_stat_ = true;
        last_write_ = mtime;
        last_size_ = size;
    }
    else
    {
        have_stat_ = false;
    }
    return adopt(true, std::move(content));
}

void KeybindingsBridge::bind_path(fs::path path)
{
    path_ = std::move(path);
    have_stat_ = false;
    (void)refresh_from_disk(); // the first read, so snapshot() is meaningful before the first poll()
}

bool KeybindingsBridge::poll()
{
    std::error_code ec;
    if (path_.empty())
    {
        return false; // permanently absent; adopt(false,"") already set at bind time
    }
    const bool is_file = fs::is_regular_file(path_, ec) && !ec;
    if (!is_file)
    {
        if (!snapshot_.present && !have_stat_)
        {
            return false; // already absent, nothing to do (the common idle case)
        }
        have_stat_ = false;
        return adopt(false, std::string());
    }
    const std::uintmax_t size = fs::file_size(path_, ec);
    if (ec)
    {
        have_stat_ = false;
        return adopt(false, std::string());
    }
    const fs::file_time_type mtime = fs::last_write_time(path_, ec);
    if (!ec && have_stat_ && snapshot_.present && mtime == last_write_ && size == last_size_)
    {
        return false; // unchanged since the last read — the cheap path that runs every idle loop
    }
    return refresh_from_disk();
}

Json KeybindingsBridge::snapshot_json() const
{
    Json out = Json::object();
    out.set("present", Json(snapshot_.present));
    out.set("generation", Json(static_cast<std::int64_t>(snapshot_.generation)));
    // Only ship the bytes when present — an absent file carries no text, and an empty string keeps the
    // renderer from mistaking a stale value for the current one.
    out.set("text", Json(snapshot_.present ? snapshot_.text : std::string()));
    return out;
}

bool KeybindingsBridge::install(BridgeRouter& router)
{
    return router.register_method(kKeybindingsGetMethod,
                                  [this](const BridgeRequest&) -> BridgeResult
                                  {
                                      ++reads_;
                                      return BridgeResult::ok(snapshot_json());
                                  });
}

} // namespace context::editor::shell
