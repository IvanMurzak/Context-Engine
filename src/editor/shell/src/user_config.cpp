// The per-user configuration store (M9 e06d) — see user_config.h for the single-writer contract, the
// merge-preserving read-modify-write rationale, and the fix for e14c's `.tmp` collision.
//
// ⚠ THIS IS THE ONLY TRANSLATION UNIT IN THE REPOSITORY THAT WRITES `~/.context/config.json`.
// `tools/check_config_writers.py` (ctest `editor-shell-config-writers`) asserts exactly that, so a
// second writer added anywhere — including a well-meaning one right here in the Shell — is a red build
// rather than a silent divergence a user discovers as a lost preference.

#include "context/editor/shell/user_config.h"

#include "context/editor/shell/keybindings_bridge.h" // home_directory() — the ONE home resolver

#include <atomic>
#include <cctype>
#include <exception>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <unistd.h> // getpid
#endif

namespace context::editor::shell
{

namespace fs = std::filesystem;
using contract::Json;

namespace
{

// The current process id, as a string. Part of the temp-file name so two concurrently-launching
// editors never stage through the same path (the e14c collision).
[[nodiscard]] std::string process_id_token()
{
#if defined(_WIN32)
    return std::to_string(::_getpid());
#else
    return std::to_string(static_cast<long long>(::getpid()));
#endif
}

// A per-process counter, so even ONE process writing twice in the same millisecond stages to distinct
// files. Atomic because the owner loop is single-threaded today but nothing in this file requires it
// to stay that way, and a counter is the cheapest possible way to not care.
std::atomic<std::uint64_t> g_write_sequence{0};

// Read a small file into `out`. False on any IO error OR when the file exceeds the cap — an oversized
// config is treated as unreadable rather than loaded, which the caller surfaces as an empty document.
[[nodiscard]] bool read_small_file(const fs::path& path, std::uintmax_t size, std::string& out)
{
    if (size > kMaxUserConfigBytes)
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
    if (stream.bad())
    {
        return false;
    }
    out = buffer.str();
    return true;
}

// Structural equality over two parsed documents, used to decide whether a re-read actually CHANGED
// anything (and therefore whether to bump the generation). Comparing the canonical dumps is exact for
// this purpose and costs nothing on a preferences-sized document — and it deliberately treats a
// cosmetic reformat as "no change", which is what keeps an editor from re-notifying itself when a user
// pretty-prints their config by hand.
[[nodiscard]] bool same_document(const Json& a, const Json& b)
{
    return a.dump(0) == b.dump(0);
}

} // namespace

// ---------------------------------------------------------------------------------- free functions

fs::path user_config_path()
{
    // Reuse the shell's single home-dir resolver (keybindings_bridge.h) — same platform rule
    // (`USERPROFILE` on Windows, `HOME` on POSIX), returning nullopt when neither is set.
    const std::optional<fs::path> home = home_directory();
    if (!home.has_value())
    {
        return {};
    }
    return *home / ".context" / "config.json";
}

Json read_user_config(const fs::path& path)
{
    Json empty = Json::object();
    if (path.empty())
    {
        return empty;
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
    {
        return empty; // absent (or a directory / special file) — an ordinary first-run state
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec)
    {
        return empty;
    }
    std::string text;
    if (!read_small_file(path, size, text))
    {
        return empty;
    }
    Json parsed;
    try
    {
        parsed = Json::parse(text);
    }
    catch (const std::exception&)
    {
        // A corrupt config reads as "nothing recorded" and is recoverable by the next write — the
        // same loudly-recoverable posture e14c's recents reader already took.
        return empty;
    }
    return parsed.is_object() ? parsed : empty;
}

fs::path staging_path_for(const fs::path& target)
{
    // A UNIQUE staging name per writer (pid + a per-process counter). The e14c writer used a FIXED
    // `<config>.tmp`, so two launches racing could publish each other's partial bytes; with distinct
    // temps the rename in `write_user_config` is once again the atomic publish it was always meant to
    // be, and the only race left is the benign last-writer-wins one a single-user preference file is
    // allowed to have.
    fs::path temp = target;
    temp += ".tmp." + process_id_token() + "." +
            std::to_string(g_write_sequence.fetch_add(1, std::memory_order_relaxed));
    return temp;
}

bool write_user_config(const fs::path& path, const Json& document, std::string* error)
{
    auto set_error = [error](std::string message)
    {
        if (error != nullptr)
        {
            *error = std::move(message);
        }
    };

    if (path.empty())
    {
        set_error("no user config path (no HOME/USERPROFILE)");
        return false;
    }
    if (!document.is_object())
    {
        set_error("the user config document must be a JSON object");
        return false;
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const fs::path temp = staging_path_for(path);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            set_error("could not open the temp config for writing");
            return false;
        }
        out << document.dump(2);
        if (!out)
        {
            set_error("write to the temp config failed");
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec)
    {
        // rename can fail across a pre-existing target on some platforms; remove + retry once.
        fs::remove(path, ec);
        std::error_code ec2;
        fs::rename(temp, path, ec2);
        if (ec2)
        {
            set_error("could not replace the config file: " + ec2.message());
            fs::remove(temp, ec);
            return false;
        }
    }
    return true;
}

bool is_valid_theme_id(const std::string& value)
{
    // Bounded, printable, and shaped like the ids editor-core mints (`builtin.dark`, `user.<stem>`).
    // The Shell cannot know the theme REGISTRY — an unknown-but-well-formed id is a legitimate state
    // (a user theme file that is not currently present), and editor-core already falls back to the
    // first-run default for an id it cannot resolve. So this validates FORM, not existence.
    if (value.empty() || value.size() > 128u)
    {
        return false;
    }
    for (const char c : value)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok = (std::isalnum(u) != 0) || c == '.' || c == '-' || c == '_';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------- the store

bool UserConfigStore::adopt(Json document)
{
    if (same_document(document, document_))
    {
        return false; // no observed change -> no generation bump
    }
    document_ = std::move(document);
    ++generation_;
    return true;
}

bool UserConfigStore::refresh_from_disk()
{
    std::error_code ec;
    if (path_.empty())
    {
        have_stat_ = false;
        return adopt(Json::object());
    }
    Json parsed = read_user_config(path_);
    // Cache the stat so an unchanged file skips the re-read on the next poll. A failed stat simply
    // means "re-read next time", which is the safe direction.
    const bool is_file = fs::is_regular_file(path_, ec) && !ec;
    if (is_file)
    {
        const std::uintmax_t size = fs::file_size(path_, ec);
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
    }
    else
    {
        have_stat_ = false;
    }
    return adopt(std::move(parsed));
}

void UserConfigStore::bind_path(fs::path path)
{
    path_ = std::move(path);
    have_stat_ = false;
    (void)refresh_from_disk(); // the first read, so document() is meaningful before the first poll()
}

bool UserConfigStore::poll()
{
    if (path_.empty())
    {
        return false; // permanently empty; adopt(object) already ran at bind time
    }
    std::error_code ec;
    const bool is_file = fs::is_regular_file(path_, ec) && !ec;
    if (!is_file)
    {
        if (!have_stat_)
        {
            return false; // already absent — the common idle case
        }
        have_stat_ = false;
        return adopt(Json::object());
    }
    const std::uintmax_t size = fs::file_size(path_, ec);
    if (ec)
    {
        have_stat_ = false;
        return refresh_from_disk();
    }
    const fs::file_time_type mtime = fs::last_write_time(path_, ec);
    if (!ec && have_stat_ && mtime == last_write_ && size == last_size_)
    {
        return false; // unchanged since the last read — the cheap path that runs every idle loop
    }
    return refresh_from_disk();
}

std::string UserConfigStore::theme() const
{
    if (!document_.is_object() || !document_.contains(kConfigThemeKey))
    {
        return {};
    }
    const Json& value = document_.at(kConfigThemeKey);
    return value.is_string() ? value.as_string() : std::string();
}

bool UserConfigStore::set(const std::string& key, const Json& value, std::string& error_code,
                          std::string& message)
{
    if (key != kConfigThemeKey)
    {
        // DENY-BY-DEFAULT over a CLOSED settable vocabulary. A request that could write any member
        // would let a compromised or buggy renderer rewrite `recents` (a list of paths on this
        // machine) or plant members a future build will read — the request/persist split would then
        // be decorative.
        ++refusals_;
        error_code = kErrConfigUnknownKey;
        message = "config.set: \"" + key + "\" is not a settable key (settable: \"" +
                  std::string(kConfigThemeKey) + "\")";
        return false;
    }
    if (!value.is_string() || !is_valid_theme_id(value.as_string()))
    {
        ++refusals_;
        error_code = kErrConfigBadValue;
        message = "config.set: \"" + std::string(kConfigThemeKey) +
                  "\" must be a short identifier string ([A-Za-z0-9._-], 1..128 chars)";
        return false;
    }

    // READ-MODIFY-WRITE over the file as it is NOW, not over the cached document: another window (or
    // the welcome screen recording a recent project) may have written since the last poll, and
    // rewriting from a stale cache would silently drop their change. Re-reading here costs one tiny
    // file read on a user-initiated action.
    Json document = read_user_config(path_);
    document.set(kConfigVersionKey, Json(kConfigVersion));
    document.set(kConfigThemeKey, value);

    std::string error;
    if (!write_user_config(path_, document, &error))
    {
        ++refusals_;
        error_code = kErrConfigWriteFailed;
        message = "config.set: could not persist the user config: " + error;
        return false;
    }
    ++writes_;
    (void)adopt(std::move(document));
    // The write we just made is the newest state; refresh the stat cache so the next poll does not
    // re-read our own change and report it as an external one.
    std::error_code ec;
    const fs::file_time_type mtime = fs::last_write_time(path_, ec);
    if (!ec)
    {
        const std::uintmax_t size = fs::file_size(path_, ec);
        if (!ec)
        {
            have_stat_ = true;
            last_write_ = mtime;
            last_size_ = size;
        }
    }
    return true;
}

Json UserConfigStore::snapshot_json() const
{
    Json out = Json::object();
    out.set("generation", Json(static_cast<std::int64_t>(generation_)));
    out.set("writable", Json(writable()));
    out.set("path", Json(path_.empty() ? std::string() : path_.generic_string()));
    // The keymap file the Settings panel points the user at (e07c). editor-core cannot resolve a home
    // directory, so the Shell reports the path even when the file does not exist yet — "here is where
    // to create it" is the useful answer for a user who has never customised their keys.
    const fs::path keybindings = default_keybindings_path();
    out.set("keybindingsPath",
            Json(keybindings.empty() ? std::string() : keybindings.generic_string()));
    out.set("config", document_);
    return out;
}

bool UserConfigStore::install(BridgeRouter& router)
{
    bool ok = router.register_method(kConfigGetMethod,
                                     [this](const BridgeRequest&) -> BridgeResult
                                     {
                                         ++reads_;
                                         return BridgeResult::ok(snapshot_json());
                                     });

    ok = router.register_method(
             kConfigSetMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string key;
                 if (request.params.is_object() && request.params.contains("key") &&
                     request.params.at("key").is_string())
                 {
                     key = request.params.at("key").as_string();
                 }
                 Json value; // null when absent — `set` refuses it as a bad value, never as a write
                 if (request.params.is_object() && request.params.contains("value"))
                 {
                     value = request.params.at("value");
                 }
                 std::string error_code;
                 std::string message;
                 if (!set(key, value, error_code, message))
                 {
                     return BridgeResult::error(error_code, message);
                 }
                 Json out = Json::object();
                 out.set("stored", Json(true));
                 out.set("key", Json(key));
                 out.set("generation", Json(static_cast<std::int64_t>(generation_)));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    return ok;
}

} // namespace context::editor::shell
