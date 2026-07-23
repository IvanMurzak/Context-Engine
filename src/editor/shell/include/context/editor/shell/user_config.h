// The per-user configuration store — `~/.context/config.json` (M9 e06d, design 06 §4 / C-F14/C-F22).
//
// WHAT THIS IS. One small JSON document holding the user's editor preferences: the chosen theme,
// the recent projects the welcome screen lists (e14c), and the window defaults. It is PER-USER, not
// per-project, and it is the ONLY thing in the system that remembers a choice across launches.
//
// ⚠ THE SHELL IS ITS SINGLE WRITER (C-F14). editor-core READS it and REQUESTS changes; the Shell
// validates and persists. That is not a layering nicety — editor-core is a pure wire-client (04 §1 /
// 08 §1) with no filesystem at all, so a "temporary" second writer could only ever be a second
// PERSISTENCE STORE (browser localStorage, a sidecar file), and the moment one exists the two
// disagree about what the user chose and nothing can say which is right. The rule is mechanised, not
// just documented: `tools/check_config_writers.py` (ctest `editor-shell-config-writers`) asserts that
// exactly ONE translation unit in this repository writes this file — this one — and that editor-core
// carries no client-side persistence API whatsoever.
//
// SO EVERY WRITE IN THE PROCESS FUNNELS THROUGH `write_user_config` BELOW, including e14c's
// recent-project recorder. That is what makes the document survive: before this task
// `record_recent_project` REPLACED the whole file with `{version, recents}`, so opening a project
// silently discarded the theme this task persists. Read-modify-write over the parsed document keeps
// members this build does not even know about — a config written by a NEWER editor is not truncated
// by an older one.
//
// THE `.tmp` COLLISION (deferred from e14c) IS FIXED HERE. The old writer staged through a fixed
// `config.json.tmp`, so two launches racing (the welcome window and a project window, the ordinary
// case) could interleave: one process's rename could publish the other's half-written bytes, or fail
// outright. The temp name now carries the process id + a per-process counter, so concurrent writers
// stage to DIFFERENT files and the rename stays the atomic publish it was meant to be. Last writer
// wins, which is the correct semantic for a single-user preference file.
//
// D10 BOUNDARY-CLEAN, like keybindings_bridge.h and for the same reason: plain std::filesystem +
// stream IO over a per-user CONFIG file, nothing from the authored-project machinery. The shell
// boundary gate's FORBIDDEN list is untouched by this module.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace context::editor::shell
{

// The bridge methods editor-core calls. Grep-stable and MIRRORED by the TS side
// (src/editor/webui/core/src/config.ts `CONFIG_GET_METHOD` / `CONFIG_SET_METHOD`), cross-checked
// byte-for-byte out of the BUILT bundle by `tools/check_webui_assets.py --panel-contract` (ctest
// `webui-panel-contract`) — the same drift gate the panel / editor-state / keybindings / themes
// surfaces ride. A rename on either side would leave the Settings panel calling a method the Shell no
// longer routes: the theme would still switch for the session and simply never persist, with NOTHING
// reporting it.
inline constexpr const char* kConfigGetMethod = "config.get";
inline constexpr const char* kConfigSetMethod = "config.set";

// The ONE settable key today (06 §4: theme selection is per-user and persists). The settable set is
// CLOSED and validated here rather than being a free-form blob write: `config.set` is a REQUEST from a
// less-privileged layer, and a request that can write arbitrary members is not a request, it is a file
// handle. `recents` and the window defaults are written by the Shell's own flows (welcome.h), never
// over the wire.
inline constexpr const char* kConfigThemeKey = "theme";

// The document member holding the recent-project list (e14c), preserved across every write.
inline constexpr const char* kConfigRecentsKey = "recents";

// The document's schema version member. Written on every store so a future migration has an anchor.
inline constexpr const char* kConfigVersionKey = "version";
inline constexpr std::int64_t kConfigVersion = 1;

// `config.set` refusal codes (R-CLI-008 shape: a stable code plus a human/AI-readable message).
inline constexpr const char* kErrConfigUnknownKey = "config.unknown_key";
inline constexpr const char* kErrConfigBadValue = "config.bad_value";
inline constexpr const char* kErrConfigWriteFailed = "config.write_failed";

// The largest config file the Shell will read. A preferences document is tiny; a file past this cap is
// treated as unreadable (an empty document) rather than pulled into memory — the same fail-closed cap
// the keybindings bridge applies, and for the same reason.
inline constexpr std::size_t kMaxUserConfigBytes = 256u * 1024u;

// `<home>/.context/config.json`, or an EMPTY path when the home directory cannot be resolved (in which
// case the store serves a permanently-empty document and every write refuses honestly).
[[nodiscard]] std::filesystem::path user_config_path();

// Read the document at `path`. ALWAYS returns an object: an absent, unreadable, oversized, malformed
// or non-object file all read as an EMPTY object, because every caller's next question is "what is the
// value of member X" and the answer for all of those is the same — "nothing was recorded". A corrupt
// file is therefore recoverable by simply writing over it, which is what the old welcome-screen reader
// already did for `recents`.
[[nodiscard]] contract::Json read_user_config(const std::filesystem::path& path);

// The staging path one write will use for `target`: `<target>.tmp.<pid>.<counter>`, a DIFFERENT name on
// every call. Exposed only so the e14c collision fix is directly provable — the defect was a FIXED
// `<target>.tmp`, and a test that could only observe the published file would pass just as happily with
// the collision still in place.
[[nodiscard]] std::filesystem::path staging_path_for(const std::filesystem::path& target);

// THE ONE WRITE PRIMITIVE (see the header). Creates the parent directory, stages to a UNIQUE temp file
// (pid + counter — the e14c `.tmp` collision), and renames it over the target so a crash mid-write
// never leaves a half-written config behind. Returns false and fills `error` (when non-null) on any
// failure; never throws.
[[nodiscard]] bool write_user_config(const std::filesystem::path& path,
                                     const contract::Json& document, std::string* error);

// Is `value` a usable theme id? A theme id is an opaque token editor-core resolves against its own
// registry (`builtin.dark`, `user.mine`), so the Shell — which cannot know the registry — validates
// only that it is a plausible, bounded identifier. Refusing here is what keeps a hostile or buggy
// renderer from writing a 10 MB "theme id" or a newline-laden one into the user's config.
[[nodiscard]] bool is_valid_theme_id(const std::string& value);

// The store: the read/watch/write owner of the document, and the `config.*` bridge surface over it.
class UserConfigStore
{
public:
    UserConfigStore() = default;

    // Non-copyable and non-movable, like the sibling bridges: `install` binds handlers capturing
    // `this`, and a router outlives nothing that could be relocated out from under it.
    UserConfigStore(const UserConfigStore&) = delete;
    UserConfigStore& operator=(const UserConfigStore&) = delete;
    UserConfigStore(UserConfigStore&&) = delete;
    UserConfigStore& operator=(UserConfigStore&&) = delete;

    // Point the store at the config file. The composition root binds `user_config_path()`; the T1
    // suite and the live smokes bind a temp file so behaviour is deterministic and no test can touch
    // the developer's real preferences. An EMPTY path means "no home directory": the document is
    // permanently empty and every `set` refuses with `config.write_failed`. Binding performs the FIRST
    // read, so `document()` is meaningful before the first `poll()`.
    void bind_path(std::filesystem::path path);

    // The watch tick, driven from the owner loop — the same cheap-stat model as the keybindings and
    // themes bridges. Re-reads only when mtime/size moved, and bumps `generation` only when the
    // observed document actually changed. Returns true when the snapshot changed this call, so a
    // second editor window's write becomes visible to this one without a restart.
    [[nodiscard]] bool poll();

    // The parsed document. Always an object (see `read_user_config`).
    [[nodiscard]] const contract::Json& document() const { return document_; }

    // The recorded theme id, or an empty string when none is recorded (a first run). Empty is what
    // sends editor-core to the `prefers-color-scheme` default (06 §4 / C-F22).
    [[nodiscard]] std::string theme() const;

    // Apply one settable key. Validates against the CLOSED vocabulary above, merges into the current
    // document (preserving every other member), and persists through `write_user_config`. On refusal
    // returns false with `error_code`/`message` set and the file untouched.
    [[nodiscard]] bool set(const std::string& key, const contract::Json& value,
                           std::string& error_code, std::string& message);

    // The `config.get` result: the document plus the facts the Settings panel needs but cannot
    // derive — where the keymap file lives (e07c, so the panel can SHOW the path a user must edit)
    // and whether this host can persist at all.
    [[nodiscard]] contract::Json snapshot_json() const;

    // Bind `config.get` + `config.set` on `router`. False when either binding was refused (a name
    // collision), which the caller must treat as a wiring bug rather than ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // --- what it saw (state + the live-smoke assertion surface) ----------------------------------
    [[nodiscard]] std::uint64_t generation() const { return generation_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    // Can this host persist at all? False only when no home directory resolved (an empty path).
    [[nodiscard]] bool writable() const { return !path_.empty(); }
    // How many times each method was served. The live settings smoke asserts `writes() >= 1` — the
    // end-to-end proof that a theme picked in the renderer reached the Shell and was persisted BY THE
    // SHELL, which is exactly the C-F14 claim.
    [[nodiscard]] std::size_t reads() const { return reads_; }
    [[nodiscard]] std::size_t writes() const { return writes_; }
    // Refused `config.set` calls (an unknown key, a bad value, or an IO failure).
    [[nodiscard]] std::size_t refusals() const { return refusals_; }

private:
    // Re-read from disk, adopting the result. Returns true when the observed document changed.
    [[nodiscard]] bool refresh_from_disk();
    [[nodiscard]] bool adopt(contract::Json document);

    std::filesystem::path path_;
    contract::Json document_ = contract::Json::object();
    std::uint64_t generation_ = 0;
    // Cheap-path stat cache — the last mtime/size read, so an unchanged file skips the re-read.
    bool have_stat_ = false;
    std::filesystem::file_time_type last_write_{};
    std::uintmax_t last_size_ = 0;
    std::size_t reads_ = 0;
    std::size_t writes_ = 0;
    std::size_t refusals_ = 0;
};

} // namespace context::editor::shell
