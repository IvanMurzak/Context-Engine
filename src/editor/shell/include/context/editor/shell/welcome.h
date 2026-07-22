// The Shell-side WELCOME surface (M9 e14c, design 07 §4 / 10 / D13): the app's front door — window 0
// on a BARE launch. It publishes a `welcome.*` bridge surface the editor-core web UI hydrates into a
// mini-welcome screen: the recent-projects list (from `~/.context/config.json`), an "Open project…"
// native folder picker, and "New from template" (a thin wrapper over the `context new` CLI, R-QA-006).
//
// WHAT DECIDES WELCOME vs PROJECT. `resolve_launch_mode` is the pure rule: a bare launch (no explicit
// `--project`) landing somewhere that is NOT already a Context project shows the welcome screen; every
// other launch opens the project. editor-core asks `welcome.state` right after the boot handshake and
// branches on the reported `mode` — and, crucially, DEFAULTS to the project path when the surface is
// absent (an `unknown_method`), so the CEF boot smokes, which install no welcome surface, are wholly
// unaffected by this seam.
//
// D10 CLEANLINESS. This lives in `context_editor_shell`, which already links `context_common` (the
// `ChildProcess` primitive used to spawn `context new` / `context edit`) and `context_client` (which
// carries `contract::Json`). No new link edge crosses the D10 shell boundary — the
// `context_assert_shell_boundary` FORBIDDEN list stays byte-identical and non-vacuous. The one piece
// that cannot be unit-tested headlessly — the native OS folder dialog — is isolated behind an
// injectable `FolderPicker` seam (real OS call in production, a deterministic fake in tests), exactly
// as the window backend and the daemon spawn path isolate their un-testable halves.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::shell
{

// --------------------------------------------------------------------------- the wire vocabulary
//
// Grep-stable, and MIRRORED by the TS side (src/editor/webui/core/src/welcome.ts). The
// `webui-welcome-contract` gate re-reads these values out of the BUILT bundle and compares them to the
// C++ constants here — the same cross-language discipline the panel / scheme / editor-state surfaces
// use, so a rename on either side reds a ctest instead of silently unbinding the welcome surface.

inline constexpr const char* kWelcomeStateMethod = "welcome.state";
inline constexpr const char* kWelcomePickFolderMethod = "welcome.pickFolder";
inline constexpr const char* kWelcomeOpenMethod = "welcome.open";
inline constexpr const char* kWelcomeNewProjectMethod = "welcome.newProject";

// The two launch-mode tokens `welcome.state` reports. Mirrored by the TS `WELCOME_MODE_*` constants
// and gated the same way (a mode token drift would silently strand the welcome branch).
inline constexpr const char* kWelcomeModeWelcome = "welcome";
inline constexpr const char* kWelcomeModeProject = "project";

// Local refusal codes (NOT R-CLI-008 catalog codes — the same rationale panel_host.h / registry.h
// state: these classify a HOST-side caller/wiring error, not a daemon-contract failure).
inline constexpr const char* kErrWelcomeBadParams = "welcome.bad_params";
inline constexpr const char* kErrWelcomeUnknownTemplate = "welcome.unknown_template";

// ------------------------------------------------------------------------------- value types

// Whether the editor booted onto the welcome screen or straight into a project (07 §4).
enum class LaunchMode
{
    welcome,
    project,
};

// The wire token for a mode (`kWelcomeMode*`). Pure.
[[nodiscard]] const char* launch_mode_token(LaunchMode mode) noexcept;

// One recent project, as `~/.context/config.json` records it and `welcome.state` reports it.
struct RecentProject
{
    std::string path;                 // absolute project directory
    std::string name;                 // display name (the leaf folder), never trusted from disk blindly
    std::int64_t last_opened_ms = 0;  // wall-clock ms; 0 when unknown
};

// One "New from template" option. The CATALOG the welcome screen offers is a thin projection of the
// `context new` templates (R-QA-006) — `label`/`description` are presentation the welcome screen owns;
// `name` is the token passed to `context new`. The `editor-shell-welcome-t2` drill validates every
// offered `name` against the REAL `context new`, so this list cannot drift from the CLI unnoticed.
struct WelcomeTemplate
{
    std::string name;
    std::string label;
    std::string description;
};

// --------------------------------------------------------------------------- pure launch decision

// True when `dir` is already a Context project (carries a `project.json`). Pure, cheap, total.
[[nodiscard]] bool is_context_project(const std::filesystem::path& dir);

// The pure D13 rule: welcome on a bare launch (no explicit `--project`) that is NOT already a project;
// project mode otherwise (an explicit project is always honored; launching INSIDE a project opens it).
[[nodiscard]] LaunchMode resolve_launch_mode(const std::filesystem::path& project,
                                             bool project_explicit);

// ------------------------------------------------------------------------ user config (recents)

// `<home>/.context/config.json` — the per-user recents store (07 §4). Resolves the home directory from
// the platform env (`USERPROFILE` on Windows, `HOME` on POSIX); returns an empty path when neither is
// set (the caller then has no recents, reported honestly rather than crashing).
[[nodiscard]] std::filesystem::path user_config_path();

// Read the recents list from a config file. TOTAL over a missing / malformed / partial file: an
// unreadable or absent config yields an EMPTY list, never an error — a first-ever launch has no recents
// and that is an ordinary state, not a failure. Only well-formed `{ "recents": [ { "path": ... } ] }`
// entries with a non-empty path are returned; anything else is skipped.
[[nodiscard]] std::vector<RecentProject> read_recent_projects(const std::filesystem::path& config_path);

// Record a project as the most-recent, writing `config_path` (creating its parent dir). Deduplicates by
// project path (an existing entry is moved to the front, not duplicated), caps the list at `max_entries`,
// and writes atomically (temp file + rename) so a crash mid-write never corrupts the store. Returns false
// (and sets `error` when non-null) only on a genuine IO failure — recording a recent is best-effort and a
// failure must never block opening a project.
[[nodiscard]] bool record_recent_project(const std::filesystem::path& config_path,
                                          const std::filesystem::path& project, std::int64_t now_ms,
                                          std::size_t max_entries = 10, std::string* error = nullptr);

// --------------------------------------------------------------------- the native folder picker

// The injectable native folder-picker seam. Returns the chosen directory, or nullopt when the user
// cancelled OR the platform has no dialog wired yet. NEVER throws.
using FolderPicker = std::function<std::optional<std::filesystem::path>()>;

// The production folder picker. Windows opens a real `IFileOpenDialog` (folder mode); other platforms
// return nullopt with an honest "not wired on this OS yet" — the packaged macOS/Linux dialogs are e15's,
// exactly as the window backend leaves non-Windows an honest gap until e12. Boundary-clean: pure OS-SDK
// calls, no engine-internal link edge.
[[nodiscard]] std::optional<std::filesystem::path> native_pick_folder();

// ------------------------------------------------------------------------- the CLI subprocess seam

// The result of running `context <args>`: the parsed R-CLI-008 envelope plus the raw text and exit code.
struct CliResult
{
    bool ran = false;             // the process spawned + exited (false => spawn/timeout failure)
    int exit_code = -1;
    contract::Json envelope;      // the parsed stdout envelope (null when unparseable)
    std::string raw;              // the raw stdout, for diagnostics
    std::string error;            // a spawn/timeout error, when ran == false

    // The envelope's top-level `ok` (false when it never ran / did not parse).
    [[nodiscard]] bool ok() const;
    // The envelope's `data.action` (`spawn` / `focused` / …) — empty when absent.
    [[nodiscard]] std::string action() const;
    // The envelope's `data.<key>` as a bool (false when absent / not a bool). Sibling of action().
    [[nodiscard]] bool data_bool(const char* key) const;
};

// The injectable `context <args>` runner. Real impl spawns the located `context` binary; tests inject a
// fake so the welcome logic is exercised without a live CLI.
using CliRunner = std::function<CliResult(const std::vector<std::string>& args)>;

// The production CLI runner over `binary` (the located `context` executable). Spawns it via the
// boundary-clean `context_common` ChildProcess primitive, drains its multi-line stdout envelope to EOF,
// waits for exit, and parses the envelope. NEVER throws.
[[nodiscard]] CliResult run_context_cli(const std::filesystem::path& binary,
                                        const std::vector<std::string>& args);

// The catalog of "New from template" options the welcome screen offers (a thin projection of the
// `context new` R-QA-006 templates). Kept honest against the CLI by the `editor-shell-welcome-t2` drill.
[[nodiscard]] const std::vector<WelcomeTemplate>& available_templates();

// ------------------------------------------------------------------------------- the bridge

// The `welcome.*` bridge surface. Mirrors the panel_host / editor_state_bridge shape: bind collaborators
// after construction (they are owned elsewhere and wired at the composition root), then `install` on the
// router. Non-copyable / non-movable, like every Shell bridge surface, because `install` binds handlers
// that capture `this`.
class WelcomeBridge
{
public:
    WelcomeBridge() = default;

    WelcomeBridge(const WelcomeBridge&) = delete;
    WelcomeBridge& operator=(const WelcomeBridge&) = delete;
    WelcomeBridge(WelcomeBridge&&) = delete;
    WelcomeBridge& operator=(WelcomeBridge&&) = delete;

    // --- configuration (before install) ---
    void set_launch_mode(LaunchMode mode) noexcept { mode_ = mode; }
    void set_project_name(std::string name) { project_name_ = std::move(name); }
    // Where recents are read from. Defaults to `user_config_path()`; tests point it at a temp file.
    void set_config_path(std::filesystem::path path) { config_path_ = std::move(path); }
    // The native folder picker. Defaults to `native_pick_folder`; tests inject a deterministic fake.
    void bind_folder_picker(FolderPicker picker) { picker_ = std::move(picker); }
    // How `context new` / `context edit` are run. Defaults to spawning `binary`; tests inject a fake.
    void bind_cli_runner(CliRunner runner) { cli_ = std::move(runner); }
    void set_cli_binary(std::filesystem::path binary) { cli_binary_ = std::move(binary); }

    // --- the method bodies, exposed for direct testing --------------------------------------------
    // Each is total over renderer-controlled params and is exactly what the corresponding `welcome.*`
    // handler calls, so the unit suite exercises the SAME code the renderer reaches.

    // `{ mode, projectName, recents:[…], templates:[…] }` — everything the welcome screen renders.
    // NOT const: it re-reads the recents store from disk and bumps the served counter each call.
    [[nodiscard]] contract::Json state();

    // Invoke the native picker. `{ picked: bool, path: string }` — `picked:false` on cancel/unsupported
    // is an ORDINARY result, not an error.
    [[nodiscard]] contract::Json pick_folder();

    // Open `path` as a project (feeds the e14a launch flow via `context edit <path>`). Returns
    // `{ opened, action, path }`; `error_code` is set (and the return is a refusal) only on a bad param.
    [[nodiscard]] bool open_project(const std::string& path, contract::Json& out,
                                    std::string& error_code);

    // Scaffold `directory` from `template_name` via `context new`, then open it. Returns
    // `{ created, runnable, directory, opened, action }`; `error_code` set on a bad param / unknown
    // template.
    [[nodiscard]] bool new_project(const std::string& directory, const std::string& template_name,
                                   contract::Json& out, std::string& error_code);

    // Bind every `welcome.*` method on `router`. False when ANY binding was refused (a name collision),
    // a wiring bug the caller must not ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // --- observation (tests / smoke) ---
    [[nodiscard]] std::size_t states_served() const noexcept { return states_served_; }
    [[nodiscard]] std::size_t folders_picked() const noexcept { return folders_picked_; }
    [[nodiscard]] std::size_t opens_served() const noexcept { return opens_served_; }
    [[nodiscard]] std::size_t new_projects_served() const noexcept { return new_projects_served_; }

private:
    // Run the CLI: the injected runner if bound, else the production spawn over `cli_binary_`.
    [[nodiscard]] CliResult run_cli(const std::vector<std::string>& args) const;

    LaunchMode mode_ = LaunchMode::welcome;
    std::string project_name_;
    std::filesystem::path config_path_ = user_config_path();
    std::filesystem::path cli_binary_;
    FolderPicker picker_;
    CliRunner cli_;

    std::size_t states_served_ = 0;
    std::size_t folders_picked_ = 0;
    std::size_t opens_served_ = 0;
    std::size_t new_projects_served_ = 0;
};

} // namespace context::editor::shell
