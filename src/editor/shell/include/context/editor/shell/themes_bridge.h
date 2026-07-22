// The watched-themes read/watch bridge surface (M9 e06b, design 06 §4 / 04 §1).
//
// WHAT THIS IS. editor-core owns the THEME ENGINE (theme.ts: tokens -> CSS custom properties, live
// switch, reduced motion, the Dockview chrome map), but it is a PURE WIRE-CLIENT (04 §1 / 08 §1) —
// it has no filesystem and CANNOT read the per-user theme files `~/.context/themes/*.theme.json`.
// Reading, WATCHING and hot-reloading them is a SHELL responsibility; this bridge does it and
// PUBLISHES the raw bytes over the SAME privileged e05c bridge (`themes.get`), which parses and
// SCHEMA-VALIDATES them against the e06a token schema.
//
// This is the DIRECTORY sibling of keybindings_bridge.h (e07c) and deliberately mirrors it
// method-for-method — same generation model, same cheap stat gate, same bytes-only split. The one
// structural difference is the unit of watching: a directory of N files rather than a single file,
// so the "did anything change" question is asked over a SORTED list of (name, mtime, size) triples
// instead of one.
//
// THE SHELL SERVES BYTES ONLY. It does not know the theme schema and never validates one; that
// discipline lives in editor-core (theme.ts `ThemeRegistry`), so a malformed theme is rejected THERE
// with a diagnostic and the previously-applied theme stands. Keeping the schema on one side is what
// makes "a bad theme is never a broken UI" a property of one code path rather than an agreement
// between two languages.
//
// D10 BOUNDARY-CLEAN, deliberately. This reads per-user CONFIG files with plain std::filesystem +
// stream IO — it does NOT touch the authored-project file machinery (context_filesync / the derived
// index), which is exactly what keeps it out of the shell-boundary gate's FORBIDDEN closure. The
// gate's FORBIDDEN list stays byte-identical.
//
// CEF-FREE, like ipc_bridge.h / editor_state_bridge.h / keybindings_bridge.h and for the same
// reason: the router handler runs nowhere the local dev gate can reach, so the read/watch logic
// lives here where the T1 suite (tests/test_themes_bridge.cpp) drives the SAME code the renderer
// reaches, on all three default `build` legs.
//
// THE WATCH. `poll()` is driven from the shell's single-threaded owner loop. It re-enumerates the
// directory (a cheap stat per file, no reads) and only when that listing differs from the last one
// does it re-read the contents; the GENERATION counter bumps when the observed content actually
// changes. editor-core reads that generation over `themes.get` and re-registers only when it moves,
// so an idle poll costs one integer compare on the renderer side.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace context::editor::shell
{

// The bridge method editor-core calls to read the watched-theme snapshot. Grep-stable and MIRRORED
// by the TS side (src/editor/webui/core/src/theme.ts `THEMES_GET_METHOD`). The `webui-panel-contract`
// gate re-reads this value out of the BUILT bundle and compares it to this constant — the same
// cross-language discipline kKeybindingsGetMethod rides: a rename on either side would leave
// editor-core calling a method the Shell no longer routes, so watched themes would silently never
// load and NOTHING would report it.
inline constexpr const char* kThemesGetMethod = "themes.get";

// The `source` token every watched file is published under. editor-core keys its registry precedence
// off this, and the SAME envelope will carry package manifest contributions (04 §3
// `Contribution::themes`) under a different token when package loading lands — which is why the
// field exists now rather than being implied.
inline constexpr const char* kThemeSourceUser = "user";

// The theme-file suffix this bridge watches. A directory entry that does not end in it is ignored
// entirely, so a README or an editor backup file in `~/.context/themes/` costs nothing.
inline constexpr const char* kThemeFileSuffix = ".theme.json";

// The largest single theme file the Shell will read and ship. A theme is a few KB of tokens; a file
// past this cap is treated as unreadable (skipped) rather than pushing a large blob through the
// response path. Comfortably above any real theme, well under the bridge's 1 MiB envelope.
inline constexpr std::size_t kMaxThemeBytes = 256u * 1024u;

// The largest number of themes served. A theme picker with hundreds of entries is a mistake, not a
// use case, and an unbounded directory would otherwise put an unbounded payload on the wire. Entries
// past the cap are dropped in sorted order, so which ones are served is deterministic.
inline constexpr std::size_t kMaxWatchedThemes = 64u;

// `<home>/.context/themes`, or an EMPTY path when the home directory cannot be resolved (in which
// case the bridge serves a permanently-empty snapshot — the editor runs on the built-in themes).
// Uses the same home resolver as keybindings_bridge.h, so the two per-user surfaces can never
// disagree about where "home" is.
[[nodiscard]] std::filesystem::path default_themes_directory();

// One watched theme as it goes on the wire. `id` is the file stem WITHOUT the `.theme.json` suffix,
// prefixed with the source (`user.solarized`), which is what editor-core registers it under.
struct WatchedTheme
{
    std::string id;
    std::string text; // the raw file bytes, UNVALIDATED (editor-core owns the schema)
};

class ThemesBridge
{
public:
    ThemesBridge() = default;

    // Non-copyable and non-movable, like the sibling bridges: `install` binds a handler capturing
    // `this`, and a router outlives nothing that could be relocated out from under it.
    ThemesBridge(const ThemesBridge&) = delete;
    ThemesBridge& operator=(const ThemesBridge&) = delete;
    ThemesBridge(ThemesBridge&&) = delete;
    ThemesBridge& operator=(ThemesBridge&&) = delete;

    // Point the bridge at the watched directory. The composition root binds
    // `default_themes_directory()`; the T1 suite binds a temp dir so the read/watch/generation
    // behaviour is deterministic. An EMPTY path means "no home directory" — the bridge then serves a
    // permanently-empty snapshot. Binding performs the FIRST enumeration so `snapshot_json()` is
    // meaningful before the first `poll()`.
    void bind_directory(std::filesystem::path directory);

    // The watch tick, driven from the owner loop. A cheap re-enumeration gates the reads: contents
    // are re-read only when the (name, mtime, size) listing moved, and `generation` is bumped only
    // when the observed CONTENT actually changes (a rewrite with identical bytes bumps nothing).
    // Returns true when the snapshot changed this call — the signal a caller (or a test) uses to
    // know a hot reload is pending on the renderer.
    [[nodiscard]] bool poll();

    // The current `{generation, themes:[{id, source, text}]}` snapshot as a JSON object (what
    // `themes.get` returns). An unreadable / oversized file is simply absent from the list.
    [[nodiscard]] contract::Json snapshot_json() const;

    // Bind `themes.get` on `router`. False when the binding was refused (a name collision), which the
    // caller must treat as a wiring bug rather than ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // --- what it saw (state + the live-smoke assertion surface) ----------------------------------
    [[nodiscard]] std::uint64_t generation() const { return generation_; }
    [[nodiscard]] const std::vector<WatchedTheme>& themes() const { return themes_; }
    [[nodiscard]] std::size_t count() const { return themes_.size(); }
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    // How many times `themes.get` was served over the router — the CEF smoke asserts this is non-zero
    // after the live renderer boots (the end-to-end proof the theme channel is wired).
    [[nodiscard]] std::size_t reads() const { return reads_; }

private:
    // The cheap-path listing entry: what a stat can tell us without opening the file.
    struct ThemeStat
    {
        std::string id;
        std::filesystem::path path;
        std::filesystem::file_time_type last_write{};
        std::uintmax_t size = 0;

        [[nodiscard]] bool operator==(const ThemeStat& other) const
        {
            return id == other.id && path == other.path && last_write == other.last_write &&
                   size == other.size;
        }
    };

    // Enumerate the watched directory into a SORTED listing. Empty when the directory is missing,
    // unreadable, or the bound path is empty — all of which read as "no watched themes".
    [[nodiscard]] std::vector<ThemeStat> enumerate() const;
    // Read every file in `listing` and adopt the result, bumping the generation iff the observed
    // content differs from the current snapshot.
    [[nodiscard]] bool adopt(std::vector<ThemeStat> listing);

    std::filesystem::path directory_;
    std::vector<WatchedTheme> themes_;
    std::vector<ThemeStat> last_listing_;
    std::uint64_t generation_ = 0;
    std::size_t reads_ = 0;
};

} // namespace context::editor::shell
