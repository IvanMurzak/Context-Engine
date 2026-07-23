// The Shell-side welcome surface (M9 e14c). See welcome.h for the design.

#include "context/editor/shell/welcome.h"

#include "context/common/child_process.h"

#include "json_number_read.h" // the shared range-guarded numeric read (float-cast-overflow UB guard)

#include <cstddef>
#include <cstdint>
#include <exception>
#include <sstream>
#include <system_error>
#include <utility>

namespace context::editor::shell
{

namespace fs = std::filesystem;
namespace process = context::common::process;
using contract::Json;

namespace
{

// Read a string member off a JSON object, defaulting on any type mismatch (total over hostile input).
[[nodiscard]] std::string read_string(const Json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key))
        return std::string();
    const Json& value = object.at(key);
    return value.is_string() ? value.as_string() : std::string();
}

// Serialize one recent-project entry to its wire object. Shared by the read-path projection (state())
// and the write-path store (record_recent_project) so the `{path,name,lastOpenedMs}` shape stays 1:1.
[[nodiscard]] Json recent_to_json(const RecentProject& recent)
{
    Json entry = Json::object();
    entry.set("path", Json(recent.path));
    entry.set("name", Json(recent.name));
    entry.set("lastOpenedMs", Json(recent.last_opened_ms));
    return entry;
}

// The display name for a project path: the leaf folder, falling back to the whole path.
[[nodiscard]] std::string project_display_name(const fs::path& project)
{
    fs::path normalized = project.lexically_normal();
    std::string leaf = normalized.filename().string();
    if (leaf.empty())
        leaf = normalized.parent_path().filename().string();
    return leaf.empty() ? normalized.string() : leaf;
}

} // namespace

// ---------------------------------------------------------------------------------- launch mode

const char* launch_mode_token(LaunchMode mode) noexcept
{
    return mode == LaunchMode::project ? kWelcomeModeProject : kWelcomeModeWelcome;
}

bool is_context_project(const fs::path& dir)
{
    std::error_code ec;
    return fs::exists(dir / "project.json", ec);
}

LaunchMode resolve_launch_mode(const fs::path& project, bool project_explicit)
{
    // An explicit --project always opens that project (file-association / `context edit .` / "Open
    // with"); a bare launch shows the welcome screen UNLESS the working directory is itself a project.
    if (project_explicit)
        return LaunchMode::project;
    return is_context_project(project) ? LaunchMode::project : LaunchMode::welcome;
}

// ------------------------------------------------------------------------------- user config

std::vector<RecentProject> read_recent_projects(const fs::path& config_path)
{
    std::vector<RecentProject> recents;
    if (config_path.empty())
        return recents;

    // ONE reader for the document (user_config.h): it is total over an absent / oversized / malformed
    // / non-object file, all of which mean the same thing here — nothing was recorded.
    const Json doc = read_user_config(config_path);
    if (!doc.contains(kConfigRecentsKey))
        return recents;
    const Json& list = doc.at(kConfigRecentsKey);
    if (!list.is_array())
        return recents;

    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const Json& entry = list.at(i);
        std::string path = read_string(entry, "path");
        if (path.empty())
            continue; // an entry with no path is not a recent
        RecentProject recent;
        recent.path = path;
        recent.name = read_string(entry, "name");
        if (recent.name.empty())
            recent.name = project_display_name(fs::path(path));
        // Range-guarded so a hostile/hand-edited lastOpenedMs cannot trip float-cast-overflow UB in
        // as_int() (the shared sanitize-leg guard — json_number_read.h); absent/out-of-range keeps the
        // 0 default. The ±9e15 bound is exactly double-representable (< 2^53) and covers any real epoch-ms.
        if (const auto ms = detail::number_in_range(entry, "lastOpenedMs", -9.0e15, 9.0e15))
            recent.last_opened_ms = static_cast<std::int64_t>(*ms);
        recents.push_back(std::move(recent));
    }
    return recents;
}

bool record_recent_project(const fs::path& config_path, const fs::path& project, std::int64_t now_ms,
                           std::size_t max_entries, std::string* error)
{
    auto set_error = [error](std::string message)
    {
        if (error != nullptr)
            *error = std::move(message);
    };

    if (config_path.empty())
    {
        set_error("no user config path (no HOME/USERPROFILE)");
        return false;
    }

    std::error_code ec;
    const fs::path normalized = project.lexically_normal();
    // weakly_canonical resolves the path without requiring it to exist; fall back to the normalized form.
    fs::path canonical = fs::weakly_canonical(normalized, ec);
    if (ec)
        canonical = normalized;
    const std::string canonical_str = canonical.generic_string();

    // Read the existing list, drop any entry for this project, then prepend the fresh one.
    std::vector<RecentProject> recents = read_recent_projects(config_path);
    RecentProject fresh;
    fresh.path = canonical_str;
    fresh.name = project_display_name(canonical);
    fresh.last_opened_ms = now_ms;

    std::vector<RecentProject> merged;
    merged.push_back(fresh);
    for (RecentProject& existing : recents)
    {
        fs::path existing_canonical = fs::weakly_canonical(fs::path(existing.path), ec);
        if (ec)
            existing_canonical = fs::path(existing.path).lexically_normal();
        if (existing_canonical.generic_string() == canonical_str)
            continue; // dedup: the fresh entry above supersedes it
        merged.push_back(std::move(existing));
        if (merged.size() >= max_entries)
            break;
    }

    // MERGE, never replace (M9 e06d). Re-read the document as it is NOW and update only the recents
    // member: `theme` — and anything a newer build records — must survive a project being opened. This
    // used to build a fresh `{version, recents}` object, which is exactly how the theme this task
    // persists would have been thrown away by the next launch.
    Json doc = read_user_config(config_path);
    doc.set(kConfigVersionKey, Json(kConfigVersion));
    Json array = Json::array();
    for (const RecentProject& recent : merged)
        array.push_back(recent_to_json(recent));
    doc.set(kConfigRecentsKey, std::move(array));

    // THE single write primitive (user_config.h): creates the parent dir and publishes through a
    // UNIQUE temp + rename, so a crash mid-write never leaves a half-written config AND two racing
    // launches cannot stage through the same `.tmp` (the defect deferred from e14c, fixed there).
    std::string write_error;
    if (!write_user_config(config_path, doc, &write_error))
    {
        set_error(std::move(write_error));
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------- CLI runner

bool CliResult::ok() const
{
    return ran && envelope.is_object() && envelope.contains("ok") && envelope.at("ok").as_bool();
}

std::string CliResult::action() const
{
    if (!envelope.is_object() || !envelope.contains("data"))
        return std::string();
    const Json& data = envelope.at("data");
    if (!data.is_object() || !data.contains("action"))
        return std::string();
    const Json& action = data.at("action");
    return action.is_string() ? action.as_string() : std::string();
}

bool CliResult::data_bool(const char* key) const
{
    if (!envelope.is_object() || !envelope.contains("data"))
        return false;
    const Json& data = envelope.at("data");
    if (!data.is_object() || !data.contains(key))
        return false;
    return data.at(key).as_bool(); // as_bool() is total: false for a missing/non-bool value
}

CliResult run_context_cli(const fs::path& binary, const std::vector<std::string>& args)
{
    CliResult result;

    process::SpawnOptions options;
    options.executable = binary;
    options.args = args;
    options.capture_stdout = true;

    std::string spawn_error;
    process::ChildProcess child = process::ChildProcess::spawn(options, spawn_error);
    if (!child.valid())
    {
        result.error = spawn_error.empty() ? "spawn failed" : spawn_error;
        return result;
    }

    // The CLI prints a multi-line pretty-printed R-CLI-008 envelope; reassemble it line by line to EOF.
    std::ostringstream out;
    std::string line;
    while (child.read_line(line, 10000))
    {
        out << line << '\n';
    }
    result.raw = out.str();

    int exit_code = 0;
    result.ran = child.wait(10000, exit_code);
    result.exit_code = exit_code;
    if (!result.ran)
    {
        result.error = "the CLI did not exit within the timeout";
        return result;
    }
    try
    {
        if (!result.raw.empty())
            result.envelope = Json::parse(result.raw);
    }
    catch (const std::exception&)
    {
        // A non-JSON stdout is surfaced via ran/exit_code + raw; envelope stays null.
    }
    return result;
}

const std::vector<WelcomeTemplate>& available_templates()
{
    // M1/R-QA-006 ships exactly one runnable template ("default"). The label/description are the
    // welcome screen's presentation; `name` is the token passed to `context new`. Extra genre templates
    // (2D platformer / 3D FPS) are a later CLI catalog task, NOT e14c — the welcome screen wraps
    // whatever `context new` actually offers, and editor-shell-welcome-t2 validates every name here
    // against the real CLI so this list cannot silently drift ahead of it.
    static const std::vector<WelcomeTemplate> templates = {
        {"default", "Empty Project",
         "A minimal runnable project — a scene, a camera, and a startable session (R-QA-006)."},
    };
    return templates;
}

// ------------------------------------------------------------------------------- the bridge

CliResult WelcomeBridge::run_cli(const std::vector<std::string>& args) const
{
    if (cli_)
        return cli_(args);
    CliResult result;
    if (cli_binary_.empty())
    {
        result.error = "no context binary configured";
        return result;
    }
    return run_context_cli(cli_binary_, args);
}

Json WelcomeBridge::state()
{
    ++states_served_;

    Json out = Json::object();
    out.set("mode", Json(std::string(launch_mode_token(mode_))));
    out.set("projectName", Json(project_name_));

    Json recents = Json::array();
    for (const RecentProject& recent : read_recent_projects(config_path_))
        recents.push_back(recent_to_json(recent));
    out.set("recents", std::move(recents));

    Json templates = Json::array();
    for (const WelcomeTemplate& tmpl : available_templates())
    {
        Json entry = Json::object();
        entry.set("name", Json(tmpl.name));
        entry.set("label", Json(tmpl.label));
        entry.set("description", Json(tmpl.description));
        templates.push_back(std::move(entry));
    }
    out.set("templates", std::move(templates));
    return out;
}

Json WelcomeBridge::pick_folder()
{
    ++folders_picked_;
    Json out = Json::object();
    std::optional<fs::path> picked = picker_ ? picker_() : native_pick_folder();
    out.set("picked", Json(picked.has_value()));
    out.set("path", Json(picked.has_value() ? picked->generic_string() : std::string()));
    return out;
}

bool WelcomeBridge::open_project(const std::string& path, Json& out, std::string& error_code)
{
    if (path.empty())
    {
        error_code = kErrWelcomeBadParams;
        return false;
    }
    ++opens_served_;

    // Feed the e14a launch flow: `context edit <path>` resolves + arbitrates single-instance + spawns
    // `context_editor --project <path>` (a new process, D15). The welcome window stays open.
    const CliResult result = run_cli({"edit", path});
    out = Json::object();
    out.set("opened", Json(result.ok()));
    out.set("action", Json(result.action()));
    out.set("path", Json(path));
    if (!result.ok() && !result.error.empty())
        out.set("error", Json(result.error));
    return true;
}

bool WelcomeBridge::new_project(const std::string& directory, const std::string& template_name,
                                Json& out, std::string& error_code)
{
    if (directory.empty())
    {
        error_code = kErrWelcomeBadParams;
        return false;
    }
    const std::string tmpl = template_name.empty() ? std::string("default") : template_name;

    bool known = false;
    for (const WelcomeTemplate& candidate : available_templates())
    {
        if (candidate.name == tmpl)
        {
            known = true;
            break;
        }
    }
    if (!known)
    {
        error_code = kErrWelcomeUnknownTemplate;
        return false;
    }
    ++new_projects_served_;

    // Thin wrapper over `context new <directory> <template>` (R-QA-006). On success, open the scaffold.
    const CliResult created = run_cli({"new", directory, tmpl});
    const bool ok = created.ok();
    const bool runnable = ok && created.data_bool("runnable");

    out = Json::object();
    out.set("created", Json(ok));
    out.set("runnable", Json(runnable));
    out.set("directory", Json(directory));
    out.set("template", Json(tmpl));

    bool opened = false;
    std::string action;
    if (ok)
    {
        const CliResult open_result = run_cli({"edit", directory});
        opened = open_result.ok();
        action = open_result.action();
    }
    out.set("opened", Json(opened));
    out.set("action", Json(action));
    if (!ok && !created.error.empty())
        out.set("error", Json(created.error));
    return true;
}

bool WelcomeBridge::install(BridgeRouter& router)
{
    bool ok = true;

    ok = router.register_method(kWelcomeStateMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(state()); }) &&
         ok;

    ok = router.register_method(kWelcomePickFolderMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(pick_folder()); }) &&
         ok;

    ok = router.register_method(
             kWelcomeOpenMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 const std::string path = read_string(request.params, "path");
                 Json out;
                 std::string error_code;
                 if (!open_project(path, out, error_code))
                     return BridgeResult::error(error_code, "welcome.open requires a non-empty 'path'");
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    ok = router.register_method(
             kWelcomeNewProjectMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 const std::string directory = read_string(request.params, "directory");
                 const std::string tmpl = read_string(request.params, "template");
                 Json out;
                 std::string error_code;
                 if (!new_project(directory, tmpl, out, error_code))
                 {
                     const char* message =
                         error_code == kErrWelcomeUnknownTemplate
                             ? "welcome.newProject: unknown template"
                             : "welcome.newProject requires a non-empty 'directory'";
                     return BridgeResult::error(error_code, message);
                 }
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    return ok;
}

} // namespace context::editor::shell
