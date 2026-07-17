// The platform export-adapter plan (see adapter.h).

#include "context/editor/build/adapter.h"

#include <string>

namespace context::editor::build
{

namespace
{

// Minimal JSON string escaping for the manifest's string fields (paths / names). The only structurally
// significant bytes in an artifact path are '"' and '\'; control chars are escaped defensively.
[[nodiscard]] std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += hex[static_cast<unsigned char>(c) & 0xF];
            }
            else
            {
                out += c;
            }
            break;
        }
    }
    return out;
}

} // namespace

bool is_known_flavor(const std::string& flavor)
{
    return flavor == kFlavorDesktop || flavor == kFlavorServer;
}

AdapterPlan plan_adapter(const std::string& target, const std::string& flavor,
                         const std::string& pack_name)
{
    AdapterPlan plan;
    plan.target = target;
    plan.flavor = flavor;
    plan.pack_name = pack_name.empty() ? "game.pack" : pack_name;
    plan.manifest_name = "context.build.json";

    // The a06/a10 adapter set covers Linux + Windows, each in desktop + server/headless flavor. Any
    // other target/flavor is an honest stub (supported=false) — the CLI reports it, never fakes an
    // artifact (R-BUILD-007).
    const bool is_linux = (target == kTargetLinux);
    const bool is_windows = (target == kTargetWindows);
    if ((!is_linux && !is_windows) || !is_known_flavor(flavor))
    {
        plan.launcher_name = "launch.sh"; // a stub echoes the posix default name; layout stays empty
        return plan;
    }

    plan.supported = true;
    plan.render_present = (flavor == kFlavorDesktop);
    // Windows ships an Authenticode-signed .exe (R-SEC-003); the POSIX launcher/manifest are unsigned
    // text either way (only the runtime binary is signed). Linux has no v1 code-signing prerequisite.
    plan.requires_signing = is_windows;

    const std::string base = plan.render_present ? kRuntimeBinaryDesktop : kRuntimeBinaryServer;
    plan.runtime_binary = is_windows ? base + kExeSuffixWindows : base;
    // cmd.exe cannot run the POSIX launch.sh, so Windows ships a launch.cmd batch launcher instead.
    plan.launcher_kind = is_windows ? LauncherKind::WindowsCmd : LauncherKind::PosixSh;
    plan.launcher_name = is_windows ? "launch.cmd" : "launch.sh";

    // The documented tarball layout (R-BUILD-005 minimal packaging): the shipped runtime binary under
    // bin/, the v1 pack under content/, the launcher at the root, and the machine-readable manifest.
    // Archive paths are always forward-slash (ustar), even for Windows.
    plan.layout.push_back({"bin/" + plan.runtime_binary, "runtime"});
    plan.layout.push_back({"content/" + plan.pack_name, "pack"});
    plan.layout.push_back({plan.launcher_name, "launcher"});
    plan.layout.push_back({plan.manifest_name, "manifest"});
    return plan;
}

std::string render_launcher(const AdapterPlan& plan)
{
    if (!plan.supported)
        return {};
    if (plan.launcher_kind == LauncherKind::WindowsCmd)
    {
        // A cmd.exe batch launcher. %~dp0 is the launcher's own directory (with a trailing backslash),
        // so the artifact runs from any extraction location on a clean Windows host (no dev tree, no
        // absolute paths baked in). `%*` forwards extra args (e.g. `--ticks N`) to the runtime. The
        // archive stores forward-slash paths; on extraction Windows presents them as bin\...\, so the
        // launcher uses backslashes. CRLF is the batch-file convention but the runtime bytes stay LF —
        // the launcher content is deterministic either way.
        std::string s;
        s += "@echo off\r\n";
        s += "rem Context export artifact launcher (M8 a10). Boots the shipped RuntimeKernel over the\r\n";
        s += "rem packed content, relative to this script's own directory. Forwards extra args.\r\n";
        s += "\"%~dp0bin\\" + plan.runtime_binary + "\" --pack \"%~dp0content\\" + plan.pack_name +
             "\" %*\r\n";
        return s;
    }
    // POSIX sh, resolves paths relative to the launcher's own directory so the artifact runs from any
    // extraction location on a clean host (no dev tree, no absolute paths baked in). Forwards extra
    // args (e.g. `--ticks N`) to the runtime. LF-only, deterministic.
    std::string s;
    s += "#!/bin/sh\n";
    s += "# Context export artifact launcher (M8 a06). Boots the shipped RuntimeKernel over the packed\n";
    s += "# content, relative to this script's own directory. Forwards extra args (e.g. --ticks N).\n";
    s += "set -e\n";
    s += "here=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n";
    s += "exec \"$here/bin/" + plan.runtime_binary + "\" --pack \"$here/content/" + plan.pack_name +
         "\" \"$@\"\n";
    return s;
}

std::string render_manifest(const AdapterPlan& plan, std::uint64_t engine_version,
                            std::uint64_t generation, std::uint64_t pack_hash)
{
    if (!plan.supported)
        return {};
    std::string s = "{\n";
    s += "  \"schema\": \"ctx:build-artifact\",\n";
    s += "  \"target\": \"" + json_escape(plan.target) + "\",\n";
    s += "  \"flavor\": \"" + json_escape(plan.flavor) + "\",\n";
    s += "  \"renderPresent\": ";
    s += plan.render_present ? "true" : "false";
    s += ",\n";
    s += "  \"runtimeBinary\": \"bin/" + json_escape(plan.runtime_binary) + "\",\n";
    s += "  \"pack\": \"content/" + json_escape(plan.pack_name) + "\",\n";
    s += "  \"launcher\": \"" + json_escape(plan.launcher_name) + "\",\n";
    // Whether the shipped runtime binary requires code-signing to ship (Windows Authenticode, R-SEC-003).
    // The launcher + manifest are unsigned text either way; only the runtime .exe is signed.
    s += "  \"requiresSigning\": ";
    s += plan.requires_signing ? "true" : "false";
    s += ",\n";
    // 64-bit values as DECIMAL STRINGS (the R-CLI-008 envelope precision rule — a JSON double loses
    // precision above 2^53).
    s += "  \"engineVersion\": \"" + std::to_string(engine_version) + "\",\n";
    s += "  \"generation\": \"" + std::to_string(generation) + "\",\n";
    s += "  \"packHash\": \"" + std::to_string(pack_hash) + "\",\n";
    // DoD 3: the artifact is deterministic MODULO the LTO link of the runtime binary.
    s += "  \"deterministicModuloLink\": true,\n";
    s += "  \"layout\": [\n";
    for (std::size_t i = 0; i < plan.layout.size(); ++i)
    {
        const ArtifactEntry& e = plan.layout[i];
        s += "    {\"path\": \"" + json_escape(e.archive_path) + "\", \"role\": \"" +
             json_escape(e.role) + "\"}";
        s += (i + 1 < plan.layout.size()) ? ",\n" : "\n";
    }
    s += "  ]\n";
    s += "}\n";
    return s;
}

} // namespace context::editor::build
