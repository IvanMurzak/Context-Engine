// `context doctor` implementation (see doctor_command.h). Probes the real host, folds it into an
// EnvironmentProbe, runs the pure context_build doctor core, and reports the R-CLI-008 envelope.
//
// Diagnostic-verb contract (the `context validate` idiom, conventions.md): `context doctor` reports the
// diagnosis THROUGH the envelope and exits 0 for a completed diagnosis — assert `data.ok`, not the
// process exit code. Only a malformed COMMAND (an unknown --target) is a non-zero usage failure
// (doctor.unknown_target). A healthy environment ⇒ data.ok == true; an incomplete one ⇒ data.ok == false
// with a top-level `code: doctor.environment_incomplete` and per-finding codes an agent branches on.

#include "context/cli/doctor_command.h"

#include "context/common/subprocess.h"
#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/import/platform_profile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace build = editor::build;
namespace import = editor::import;
namespace subprocess = context::common::subprocess;
namespace fs = std::filesystem;

namespace
{

[[nodiscard]] std::optional<std::string> flag(const std::map<std::string, std::string>& flags,
                                              const std::string& name)
{
    const auto it = flags.find(name);
    if (it == flags.end())
        return std::nullopt;
    return it->second;
}

// True when the environment variable `name` is set to a NON-EMPTY value. PRESENCE ONLY — the VALUE is
// never read out, logged, or returned (R-BUILD-008: the doctor never surfaces a secret/credential). MSVC
// rejects std::getenv as C4996 under /W4 /WX, so the _MSC_VER branch uses getenv_s (its length-only form
// touches no value); the local MinGW/GCC gate + the Clang CI legs compile the std::getenv branch, so the
// branch the local gate exercises is the standard one.
[[nodiscard]] bool env_present(const char* name)
{
#if defined(_MSC_VER)
    std::size_t len = 0;
    return ::getenv_s(&len, nullptr, 0, name) == 0 && len > 0;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
#endif
}

// This host's native build target (the one a plain `context doctor` validates). Derived from the ONE
// data-driven host-platform seam (import::host_platform_profile) — the build-target ids ARE the
// import::platform_profiles ids (see toolchain_manifest.h), so this tracks the single source of truth
// instead of re-deriving the host platform behind a local #if that gets no local CI compile signal.
std::string host_native_target()
{
    return import::host_platform_profile().id;
}

// Extract the first dotted numeric version (>= major.minor) from a tool's --version output. "cmake
// version 3.29.2" -> "3.29.2"; "clang version 20.1.2" -> "20.1.2"; "v20.11.0" -> "20.11.0". "" when none.
std::string extract_version(const std::string& text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] >= '0' && text[i] <= '9')
        {
            std::size_t j = i;
            bool saw_dot = false;
            while (j < text.size() &&
                   ((text[j] >= '0' && text[j] <= '9') || text[j] == '.'))
            {
                if (text[j] == '.')
                    saw_dot = true;
                ++j;
            }
            std::string cand = text.substr(i, j - i);
            while (!cand.empty() && cand.back() == '.')
                cand.pop_back();
            if (saw_dot && cand.find('.') != std::string::npos)
                return cand;
            i = j;
        }
        else
        {
            ++i;
        }
    }
    return std::string();
}

// Run `<exe> <args>` (best-effort), capturing stdout+stderr, and parse a version. Presence keys off a
// parsed version (some tools — e.g. `cl` with no input file — exit non-zero yet print a version banner),
// falling back to "ran with output" so a version-less-but-present tool is still reported present.
build::ToolProbe probe_tool(const std::string& name, const std::string& exe, const std::string& args)
{
    build::ToolProbe t;
    t.name = name;
    const fs::path out = subprocess::make_scratch_path("ctx-doctor", ".txt");
    subprocess::ScratchFile guard(out);
    std::string cmd;
    try
    {
        cmd = subprocess::quote_argument(exe);
        if (!args.empty())
            cmd += " " + args;
        cmd += " > " + subprocess::quote_argument(out.string()) + " 2>&1";
    }
    catch (const subprocess::MetacharacterError&)
    {
        return t; // a tool name/arg with a metacharacter is a defect, not a probe result
    }
    const int rc = subprocess::run_command(cmd);
    const std::string output = subprocess::read_file(out);
    const std::string version = extract_version(output);
    if (!version.empty())
    {
        t.present = true;
        t.version = version;
    }
    else if (rc == 0 && !output.empty())
    {
        t.present = true; // ran cleanly but no version parsed — present, version unknown
    }
    return t;
}

// The probe command for a manifest component id. apple-clang is the `clang` binary; msvc is `cl` (its
// banner prints on a bare invocation). A component this table does not know is not probed (absent).
bool probe_command_for(const std::string& component, std::string& exe, std::string& args)
{
    if (component == "clang" || component == "apple-clang")
    {
        exe = "clang";
        args = "--version";
        return true;
    }
    if (component == "emscripten-clang")
    {
        exe = "emcc";
        args = "--version";
        return true;
    }
    if (component == "msvc")
    {
        exe = "cl";
        args = "";
        return true;
    }
    if (component == "cmake")
    {
        exe = "cmake";
        args = "--version";
        return true;
    }
    if (component == "node")
    {
        exe = "node";
        args = "--version";
        return true;
    }
    return false;
}

// Read the per-user inotify watch limit (Linux). std::ifstream on the /proc pseudo-file is portable: on a
// non-Linux host the path does not exist, the stream fails, and we report -1 (unknown) — no platform-#if.
std::int64_t read_watch_limit()
{
    std::ifstream in("/proc/sys/fs/inotify/max_user_watches");
    if (!in)
        return -1;
    std::int64_t value = -1;
    in >> value;
    if (!in || value < 0)
        return -1;
    return value;
}

// Count the regular files under `project` the daemon would watch, capped so a huge checkout can't stall
// the probe. Skips the heavy build/VCS/cache dirs. -1 when the path is absent/unreadable.
std::int64_t count_project_files(const fs::path& project)
{
    std::error_code ec;
    if (!fs::exists(project, ec) || !fs::is_directory(project, ec))
        return -1;
    constexpr std::int64_t kCap = 200000;
    std::int64_t count = 0;
    fs::recursive_directory_iterator it(project, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return -1;
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec))
    {
        if (ec)
            break;
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        if (it->is_directory(ec) &&
            (name == ".git" || name == "build" || name == "out" || name == "node_modules" ||
             name == "vcpkg_installed" || name == ".editor"))
        {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec))
        {
            if (++count >= kCap)
                break;
        }
    }
    return count;
}

// Split a comma-separated --target value, expanding `all` to the v1 target set, order-preserving + deduped.
std::vector<std::string> parse_targets(const std::string& raw)
{
    std::vector<std::string> out;
    const auto add = [&out](const std::string& t) {
        if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end())
            out.push_back(t);
    };
    std::stringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        // trim surrounding whitespace
        const std::size_t b = tok.find_first_not_of(" \t");
        const std::size_t e = tok.find_last_not_of(" \t");
        if (b == std::string::npos)
            continue;
        const std::string t = tok.substr(b, e - b + 1);
        if (t == "all")
            for (const import::PlatformProfile& p : import::platform_profiles()) // the v1 target set
                add(p.id);
        else
            add(t);
    }
    return out;
}

Json component_json(const build::ComponentFinding& c)
{
    Json j = Json::object();
    j.set("target", Json(c.target));
    j.set("component", Json(c.component));
    j.set("role", Json(c.role));
    j.set("acquisition", Json(c.acquisition));
    j.set("status", Json(c.status));
    j.set("requiredVersion", Json(c.required_version));
    j.set("foundVersion", Json(c.found_version));
    j.set("enforcement", Json(c.enforcement));
    j.set("fetchable", Json(c.fetchable));
    j.set("canFetchNow", Json(c.can_fetch_now));
    j.set("blocking", Json(c.blocking));
    j.set("code", Json(c.code));
    j.set("remediation", Json(c.remediation));
    j.set("remediationPointer", Json(c.remediation_pointer));
    return j;
}

Json filesync_json(const build::FileSyncFinding& f)
{
    Json j = Json::object();
    j.set("projectFileCount", Json(f.project_file_count));
    j.set("worktreeDaemonCount", Json(f.worktree_daemon_count));
    j.set("watchLimit", Json(f.watch_limit));
    j.set("fdLimit", Json(f.fd_limit));
    j.set("requiredWatches", Json(f.required_watches));
    j.set("status", Json(f.status));
    j.set("blocking", Json(f.blocking));
    j.set("code", Json(f.code));
    j.set("remediation", Json(f.remediation));
    j.set("remediationPointer", Json(f.remediation_pointer));
    return j;
}

Json signing_json(const build::SigningFinding& s)
{
    Json j = Json::object();
    j.set("target", Json(s.target));
    j.set("requirement", Json(s.requirement));
    j.set("status", Json(s.status));
    j.set("blocking", Json(s.blocking));
    j.set("code", Json(s.code));
    j.set("remediation", Json(s.remediation));
    j.set("remediationPointer", Json(s.remediation_pointer));
    return j;
}

} // namespace

Json doctor_report_json(const build::DoctorReport& report, bool fetch_offered)
{
    Json data = Json::object();
    data.set("ok", Json(report.ok));
    if (!report.ok)
    {
        data.set("code", Json(std::string(build::kDoctorEnvironmentIncompleteCode)));
        data.set("summary",
                 Json(std::string("one or more required toolchain components are missing or the wrong "
                                  "version for a requested target — see components[].")));
    }

    Json targets = Json::array();
    for (const std::string& t : report.targets)
        targets.push_back(Json(t));
    data.set("targets", std::move(targets));

    Json components = Json::array();
    for (const build::ComponentFinding& c : report.components)
        components.push_back(component_json(c));
    data.set("components", std::move(components));

    data.set("fileSyncBudget", filesync_json(report.filesync));

    Json signing = Json::array();
    for (const build::SigningFinding& s : report.signing)
        signing.push_back(signing_json(s));
    data.set("signing", std::move(signing));

    // The --fetch offer (R-BUILD-008 "can be fetched now"): enumerate the fetchable components that are
    // missing/mismatched, machine-readably. v1 reports the OFFER; the live fetch-and-verify runs through
    // the a08-verified fetch path (docs/signing.md) — a documented seam, honestly not auto-executed here.
    if (fetch_offered)
    {
        Json fetch = Json::object();
        fetch.set("offered", Json(true));
        fetch.set("note",
                  Json(std::string("each fetchable component below can be fetched now via the "
                                   "a08-verified fetch path (verify-before-use, fail-closed, R-SEC-009); "
                                   "the live fetcher is the documented R-BUILD-008 fetch seam.")));
        Json fetchable = Json::array();
        for (const build::ComponentFinding& c : report.components)
            if (c.fetchable && c.status != "ok")
            {
                Json entry = Json::object();
                entry.set("target", Json(c.target));
                entry.set("component", Json(c.component));
                entry.set("status", Json(c.status));
                fetchable.push_back(std::move(entry));
            }
        fetch.set("components", std::move(fetchable));
        data.set("fetch", std::move(fetch));
    }

    Json warnings = Json::array();
    for (const std::string& w : report.warnings)
        warnings.push_back(Json(w));
    data.set("warnings", std::move(warnings));
    return data;
}

Envelope run_doctor(const std::map<std::string, std::string>& flags)
{
    // --- resolve the requested target(s) --------------------------------------------------------
    const std::string raw_target = flag(flags, "target").value_or(host_native_target());
    const std::vector<std::string> targets = parse_targets(raw_target);
    if (targets.empty())
        return Envelope::failure(std::string(build::kDoctorUnknownTargetCode),
                                 "no build target resolved from --target '" + raw_target + "'");
    for (const std::string& t : targets)
        if (build::toolchain_target_key(t).empty())
            return Envelope::failure(std::string(build::kDoctorUnknownTargetCode),
                                     "unknown build target '" + t +
                                         "' (expected windows | linux | macos | web | all)",
                                     t);

    // --- probe the host toolchain: the union of every requested target's required components -----
    const std::vector<build::ToolchainEntry>& manifest = build::toolchain_manifest();
    build::EnvironmentProbe probe;
    std::vector<std::string> probed; // component names already probed (probe once, reuse)
    for (const std::string& target : targets)
        for (const build::ComponentRequirement& req : build::component_requirements(target, manifest))
        {
            if (std::find(probed.begin(), probed.end(), req.component) != probed.end())
                continue;
            probed.push_back(req.component);
            std::string exe;
            std::string args;
            if (probe_command_for(req.component, exe, args))
                probe.tools.push_back(probe_tool(req.component, exe, args));
            else
                probe.tools.push_back(build::ToolProbe{req.component, false, std::string()});
        }

    // --- probe the file-sync OS resource budget (R-FILE-002 up-front watcher.degraded check) -----
    const fs::path project(flag(flags, "project").value_or("."));
    probe.filesync.project_file_count = count_project_files(project);
    probe.filesync.worktree_daemon_count = 1; // v1: the current daemon (the core supports N)
    probe.filesync.watch_limit = read_watch_limit();
    probe.filesync.fd_limit = -1; // v1: the open-fd cap is a documented probe gap (unknown)

    // Signing prerequisites (R-BUILD-005 / R-BUILD-008, a10): a PRESENCE-ONLY probe per requested
    // target's requirement. For Windows Authenticode, "configured" = a signing identity is present in the
    // environment — the Azure Trusted Signing service principal (AZURE_CLIENT_ID, the primary path) OR a
    // developer-supplied Authenticode identity (CONTEXT_SIGNING_IDENTITY, the fallback). PRESENCE ONLY:
    // env_present reads no secret VALUE, so the doctor reports "configured"/"absent" without ever
    // surfacing a credential. macOS notarization has no v1 probe here (a13 scope), so it stays "unknown".
    for (const std::string& target : targets)
        for (const std::string& requirement : build::signing_requirements(target))
        {
            if (requirement == "authenticode")
            {
                const bool configured =
                    env_present("AZURE_CLIENT_ID") || env_present("CONTEXT_SIGNING_IDENTITY");
                probe.signing.push_back(build::SigningProbe{target, requirement, configured,
                                                            /*known=*/true});
            }
            // else: no v1 presence probe for this requirement — the core reports it "unknown".
        }

    const build::DoctorReport report = build::diagnose(targets, manifest, probe);
    const bool fetch_offered = flags.find("fetch") != flags.end();

    // Diagnostic-verb contract: a completed diagnosis ALWAYS returns success (exit 0); assert data.ok.
    Envelope env = Envelope::success(doctor_report_json(report, fetch_offered));
    for (const std::string& w : report.warnings)
        env.add_warning(w);
    return env;
}

} // namespace context::cli
