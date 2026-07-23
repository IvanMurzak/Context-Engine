// The watched-themes read/watch bridge surface (M9 e06b) — see themes_bridge.h for the D10
// boundary-clean rationale, the bytes-only split (the schema lives in editor-core), and the
// generation watch model.

#include "context/editor/shell/themes_bridge.h"

#include "context/editor/shell/keybindings_bridge.h" // home_directory() — one home resolver, not two

#include <algorithm>
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

// Read a small file into `out` (binary, so bytes round-trip). False on any IO error OR when the file
// exceeds the cap — an oversized theme is treated as unreadable rather than shipped, which surfaces
// as that theme simply not being in the list (the others still load). `size` is the already-stat'd
// size, passed in so this does not re-stat.
[[nodiscard]] bool read_small_file(const fs::path& path, std::uintmax_t size, std::string& out)
{
    if (size > kMaxThemeBytes)
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

// `solarized.theme.json` -> `user.solarized`, or an EMPTY string when the name does not end in the
// watched suffix (or is nothing but the suffix). The wire id carries the source prefix so editor-core
// can tell a user file from a future package contribution without a second field lookup.
[[nodiscard]] std::string theme_id_from_filename(const std::string& filename)
{
    const std::string suffix(kThemeFileSuffix);
    if (filename.size() <= suffix.size())
    {
        return {};
    }
    if (filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
    {
        return {};
    }
    return std::string(kThemeSourceUser) + "." + filename.substr(0, filename.size() - suffix.size());
}

} // namespace

fs::path default_themes_directory()
{
    const std::optional<fs::path> home = home_directory();
    if (!home.has_value())
    {
        return {}; // no home -> an empty path -> a permanently-empty snapshot (built-ins stand)
    }
    return *home / ".context" / "themes";
}

// -------------------------------------------------------------------------------- the bridge

std::vector<ThemesBridge::ThemeStat> ThemesBridge::enumerate() const
{
    std::vector<ThemeStat> listing;
    if (directory_.empty())
    {
        return listing;
    }
    std::error_code ec;
    fs::directory_iterator it(directory_, fs::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        return listing; // missing or unreadable directory -> no watched themes, not an error
    }
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            break; // the directory changed under us; the next poll re-enumerates cleanly
        }
        const fs::directory_entry& entry = *it;
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec)
        {
            continue;
        }
        // `u8string`-free on purpose: the id is a wire string and `string()` is what every other
        // path-to-wire conversion in the Shell already uses, so encodings cannot disagree between
        // surfaces.
        const std::string id = theme_id_from_filename(entry.path().filename().string());
        if (id.empty())
        {
            continue; // not a *.theme.json — a README or a backup file costs nothing
        }
        ThemeStat stat;
        stat.id = id;
        stat.path = entry.path();
        stat.size = entry.file_size(entry_ec);
        if (entry_ec)
        {
            continue;
        }
        stat.last_write = entry.last_write_time(entry_ec);
        if (entry_ec)
        {
            stat.last_write = fs::file_time_type{};
        }
        listing.push_back(std::move(stat));
    }
    // SORTED BY ID so the wire order — and therefore which entries survive the cap — is deterministic
    // rather than filesystem-order-dependent.
    std::sort(listing.begin(), listing.end(),
              [](const ThemeStat& lhs, const ThemeStat& rhs) { return lhs.id < rhs.id; });
    if (listing.size() > kMaxWatchedThemes)
    {
        listing.resize(kMaxWatchedThemes);
    }
    return listing;
}

bool ThemesBridge::adopt(std::vector<ThemeStat> listing)
{
    std::vector<WatchedTheme> read;
    read.reserve(listing.size());
    for (const ThemeStat& stat : listing)
    {
        std::string content;
        if (!read_small_file(stat.path, stat.size, content))
        {
            continue; // unreadable / oversized -> simply absent; the rest of the directory loads
        }
        read.push_back(WatchedTheme{stat.id, std::move(content)});
    }
    last_listing_ = std::move(listing);
    const bool changed =
        read.size() != themes_.size() ||
        !std::equal(read.begin(), read.end(), themes_.begin(),
                    [](const WatchedTheme& lhs, const WatchedTheme& rhs)
                    { return lhs.id == rhs.id && lhs.text == rhs.text; });
    if (!changed)
    {
        return false; // a byte-identical rewrite (or a touch) is not an observed change
    }
    themes_ = std::move(read);
    ++generation_;
    return true;
}

void ThemesBridge::bind_directory(fs::path directory)
{
    directory_ = std::move(directory);
    last_listing_.clear();
    (void)adopt(enumerate()); // the first read, so snapshot() is meaningful before the first poll()
}

bool ThemesBridge::poll()
{
    std::vector<ThemeStat> listing = enumerate();
    if (listing.size() == last_listing_.size() &&
        std::equal(listing.begin(), listing.end(), last_listing_.begin()))
    {
        return false; // unchanged since the last enumeration — the cheap path every idle loop takes
    }
    return adopt(std::move(listing));
}

Json ThemesBridge::snapshot_json() const
{
    Json out = Json::object();
    out.set("generation", Json(static_cast<std::int64_t>(generation_)));
    Json themes = Json::array();
    for (const WatchedTheme& theme : themes_)
    {
        Json entry = Json::object();
        entry.set("id", Json(theme.id));
        entry.set("source", Json(std::string(kThemeSourceUser)));
        entry.set("text", Json(theme.text));
        themes.push_back(std::move(entry));
    }
    out.set("themes", std::move(themes));
    return out;
}

bool ThemesBridge::install(BridgeRouter& router)
{
    return router.register_method(kThemesGetMethod,
                                  [this](const BridgeRequest&) -> BridgeResult
                                  {
                                      ++reads_;
                                      return BridgeResult::ok(snapshot_json());
                                  });
}

} // namespace context::editor::shell
