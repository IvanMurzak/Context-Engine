// `context edit .` project-open entry path implementation (see edit_open.h).

#include "context/cli/edit_open.h"

#include "context/cli/args.h" // flag_value / has_flag / parse_u64
#include "context/common/child_process.h"
#include "context/editor/client/arbitration.h"
#include "context/editor/contract/json.h"

#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace context::cli
{

namespace fs = std::filesystem;
using editor::contract::Envelope;
using editor::contract::Json;
namespace client = context::editor::client;

namespace
{

// How long the opener waits for a live editor to consume its focus request before concluding the marker
// is stale and a new editor must be launched. Long enough for an owner-loop poll cycle, short enough
// that a stale marker does not stall an open for a noticeable beat.
constexpr int kDefaultFocusTimeoutMs = 1500;

// The `edit` command's ONLY value-taking flag; every other `--flag` is a bare presence flag (notably
// `--no-launch`). Without this distinction a bare flag would wrongly swallow the project positional as
// its "value" (`context edit --no-launch .` would then see no directory).
[[nodiscard]] bool flag_takes_value(const std::string& flag_name)
{
    return flag_name == "focus-timeout-ms";
}

// The flag name of a `--name` / `--name=value` token (the token starts with "--").
[[nodiscard]] std::string flag_name_of(const std::string& tok)
{
    const std::string body = tok.substr(2);
    const std::size_t eq = body.find('=');
    return eq == std::string::npos ? body : body.substr(0, eq);
}

// Every non-flag positional, correctly skipping a value-flag's separate value token.
std::vector<std::string> positionals_of(const std::vector<std::string>& args)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& tok = args[i];
        if (tok.rfind("--", 0) == 0)
        {
            const bool inline_value = tok.find('=') != std::string::npos;
            if (!inline_value && flag_takes_value(flag_name_of(tok)) && (i + 1) < args.size())
            {
                ++i; // the following token is this flag's value, not a positional
            }
            continue;
        }
        out.push_back(tok);
    }
    return out;
}

std::optional<std::string> first_positional(const std::vector<std::string>& args)
{
    const std::vector<std::string> pos = positionals_of(args);
    if (pos.empty())
        return std::nullopt;
    return pos.front();
}

// Resolve a project argument to an absolute path. weakly_canonical over an existing dir yields the real
// path; on any error fall back to absolute() so the value is still usable + reportable.
fs::path resolve_project(const std::string& arg)
{
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::path(arg), ec);
    if (ec || resolved.empty())
    {
        std::error_code ec2;
        resolved = fs::absolute(fs::path(arg), ec2);
        if (ec2)
            resolved = fs::path(arg);
    }
    return resolved;
}

} // namespace

bool is_edit_open_shape(const std::vector<std::string>& args_after_edit)
{
    // The project-open shape is exactly ONE positional that names a directory (or `.`/`..`). The
    // two-positional `edit <file> <content>` write and a lone file positional are NOT this shape and
    // stay the reserved operational verb.
    const std::vector<std::string> pos = positionals_of(args_after_edit);
    if (pos.size() != 1)
        return false;
    if (pos.front() == "." || pos.front() == "..")
        return true;
    std::error_code ec;
    return fs::is_directory(fs::path(pos.front()), ec);
}

fs::path locate_editor_binary(const fs::path& self_exe)
{
    const fs::path dir =
        self_exe.has_parent_path() ? self_exe.parent_path() : fs::current_path();
#if defined(_WIN32)
    const char* exe = "context_editor.exe";
#else
    const char* exe = "context_editor";
#endif
    const fs::path candidates[] = {
        dir / exe,                                    // install layout: siblings in one bin dir
        dir / ".." / "editor" / "shell" / exe,        // dev build tree: <build>/cli -> <build>/editor/shell
        dir / ".." / ".." / "editor" / "shell" / exe, // a deeper cli/ nesting, mirrored
    };
    for (const fs::path& candidate : candidates)
    {
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate.lexically_normal();
    }
    return (dir / exe).lexically_normal(); // best guess; the spawn reports a clear error if absent
}

Envelope run_edit_open(const std::vector<std::string>& args_after_edit, const fs::path& self_exe)
{
    const std::optional<std::string> project_arg = first_positional(args_after_edit);
    if (!project_arg.has_value())
        return Envelope::failure("usage.missing_argument",
                                 "usage: context edit <project-dir>  (e.g. context edit .)");

    const bool no_launch = has_flag(args_after_edit, "no-launch");
    int focus_timeout_ms = kDefaultFocusTimeoutMs;
    if (const std::optional<std::string> t = flag_value(args_after_edit, "focus-timeout-ms"))
    {
        // Strict parse + range-check (like daemon_command's --crawl-interval-ms): a value above
        // INT_MAX would wrap to a NEGATIVE int in the cast below, so the arbitration deadline is
        // already in the past and a LIVE editor is never given time to acknowledge — spawning a
        // DUPLICATE, the exact outcome this single-instance arbitration exists to prevent. Reject it
        // as a usage error rather than silently wrap the safety net off.
        const std::optional<std::uint64_t> parsed = parse_u64(*t);
        if (!parsed.has_value() ||
            *parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            return Envelope::failure("usage.invalid",
                                     "--focus-timeout-ms expects a non-negative integer, got '" + *t +
                                         "'");
        focus_timeout_ms = static_cast<int>(*parsed);
    }

    const fs::path project = resolve_project(*project_arg);
    std::error_code ec;
    if (!fs::is_directory(project, ec))
        return Envelope::failure("usage.invalid",
                                 "not a project directory: '" + project.string() + "'");

    const client::OpenArbitration arb = client::arbitrate_open(project, focus_timeout_ms);

    Json data = Json::object();
    data.set("project", Json(project.generic_string()));
    data.set("markerPresent", Json(arb.marker_present));

    if (arb.action == client::OpenAction::focus_existing)
    {
        // Single-instance: the running editor was focused via the handshake — do NOT launch a second.
        data.set("action", Json(std::string("focused")));
        data.set("existingPid", Json(arb.existing_pid));
        return Envelope::success(std::move(data));
    }

    // spawn_new: no live editor for this project (absent or stale marker) — launch one, feeding e14a.
    data.set("action", Json(std::string("spawn")));
    if (no_launch)
    {
        data.set("launched", Json(false));
        data.set("reason", Json(std::string("--no-launch")));
        return Envelope::success(std::move(data));
    }

    const fs::path editor_binary = locate_editor_binary(self_exe);
    common::process::SpawnOptions options;
    options.executable = editor_binary;
    options.args = {"--project", project.string()};
    options.capture_stdout = false; // fire-and-forget: the editor owns its own stdout
    std::string spawn_error;
    common::process::ChildProcess child =
        common::process::ChildProcess::spawn(options, spawn_error);
    if (!child.valid())
    {
        // REPORTED, not fatal to the decision: the arbitration still concluded "spawn", but the launch
        // itself failed (e.g. the editor binary is not where we looked). Surface it so a caller/user can
        // act, without pretending an editor came up.
        data.set("launched", Json(false));
        data.set("editorBinary", Json(editor_binary.generic_string()));
        Envelope env = Envelope::success(std::move(data));
        env.add_warning("could not launch the editor: " + spawn_error);
        return env;
    }
    // Relinquish ownership: the editor must outlive this short-lived opener process.
    const std::int64_t editor_pid = child.pid();
    child.detach();

    data.set("launched", Json(true));
    data.set("editorBinary", Json(editor_binary.generic_string()));
    data.set("editorPid", Json(editor_pid));
    return Envelope::success(std::move(data));
}

} // namespace context::cli
