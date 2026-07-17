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

    // The web adapter (a11): the Emscripten/emdawnwebgpu web export bundle — the shipped `.wasm` module
    // + its `.js` glue under bin/, an index.html shell launcher, the v1 pack under content/ (streamed by
    // HTTP range requests to the browser RuntimeKernel), and the manifest. WebGPU-only (render always
    // present, L-56); web is never desktop/server-flavored (a headless browser build is nonsensical), so
    // only the desktop flavor is supported — web + server is the honest stub. No native launcher, no
    // code-signing (v1).
    if (target == kTargetWeb)
    {
        if (flavor != kFlavorDesktop)
        {
            plan.launcher_name = kWebLauncherName; // a stub echoes the web launcher name; layout empty
            return plan;
        }
        plan.supported = true;
        plan.render_present = true;   // WebGPU-only — the browser build is inherently render-present
        plan.requires_signing = false; // no v1 web code-signing (unlike the Windows Authenticode axis)
        plan.runtime_binary = kRuntimeBinaryWeb; // context-runtime.wasm
        plan.runtime_loader = kRuntimeLoaderWeb; // context-runtime.js (the Emscripten glue)
        plan.launcher_kind = LauncherKind::WebHtml;
        plan.launcher_name = kWebLauncherName;   // index.html
        // The web bundle layout (R-BUILD-005 minimal packaging, adapted for static hosting): the wasm
        // module + its JS loader under bin/, the HTML shell at the root, the pack under content/, and
        // the manifest. Archive paths are always forward-slash.
        plan.layout.push_back({"bin/" + plan.runtime_binary, kRoleRuntime});
        plan.layout.push_back({"bin/" + plan.runtime_loader, kRoleRuntimeLoader});
        plan.layout.push_back({plan.launcher_name, kRoleLauncher});
        plan.layout.push_back({"content/" + plan.pack_name, kRolePack});
        plan.layout.push_back({plan.manifest_name, kRoleManifest});
        return plan;
    }

    // The a06/a10/a13 adapter set covers Linux + Windows + macOS, each in desktop + server/headless
    // flavor. Any other target/flavor is an honest stub (supported=false) — the CLI reports it, never
    // fakes an artifact (R-BUILD-007).
    const bool is_linux = (target == kTargetLinux);
    const bool is_windows = (target == kTargetWindows);
    const bool is_macos = (target == kTargetMacos);
    if ((!is_linux && !is_windows && !is_macos) || !is_known_flavor(flavor))
    {
        plan.launcher_name = "launch.sh"; // a stub echoes the posix default name; layout stays empty
        return plan;
    }

    plan.supported = true;
    plan.render_present = (flavor == kFlavorDesktop);
    // Windows (Authenticode) and macOS (Developer ID + notarization, a13) ship a code-signed runtime
    // binary (R-SEC-003); the launcher/manifest are unsigned text either way (only the runtime binary is
    // signed). Linux has no v1 code-signing prerequisite.
    plan.requires_signing = is_windows || is_macos;

    const std::string base = plan.render_present ? kRuntimeBinaryDesktop : kRuntimeBinaryServer;
    // Only Windows appends `.exe`; macOS + Linux ship a suffix-less Mach-O/ELF binary.
    plan.runtime_binary = is_windows ? base + kExeSuffixWindows : base;
    // cmd.exe cannot run the POSIX launch.sh, so Windows ships a launch.cmd batch launcher instead; macOS
    // + Linux are POSIX and ship the same launch.sh.
    plan.launcher_kind = is_windows ? LauncherKind::WindowsCmd : LauncherKind::PosixSh;
    plan.launcher_name = is_windows ? "launch.cmd" : "launch.sh";

    // The documented tarball layout (R-BUILD-005 minimal packaging): the shipped runtime binary under
    // bin/, the v1 pack under content/, the launcher at the root, and the machine-readable manifest.
    // Archive paths are always forward-slash (ustar), even for Windows.
    plan.layout.push_back({"bin/" + plan.runtime_binary, kRoleRuntime});
    plan.layout.push_back({"content/" + plan.pack_name, kRolePack});
    plan.layout.push_back({plan.launcher_name, kRoleLauncher});
    plan.layout.push_back({plan.manifest_name, kRoleManifest});
    return plan;
}

std::string render_launcher(const AdapterPlan& plan)
{
    if (!plan.supported)
        return {};
    if (plan.launcher_kind == LauncherKind::WebHtml)
    {
        // The web export shell (a11). A static index.html that boots the Emscripten module over the
        // browser's WebGPU and streams content/<pack> by HTTP range requests, all page-relative so the
        // bundle serves from any static host with no absolute paths baked in. The Module.arguments carry
        // `--pack content/<pack>` (the same contract the native launchers pass), and the canvas is the
        // WebGPU present surface. Deterministic (a pure function of the plan); LF-only. NB the fixed
        // heap (-sINITIAL_MEMORY, NO -sALLOW_MEMORY_GROWTH) is a COMPILE-time property of the shipped
        // .wasm/.js pair, not the shell — see docs/export-adapters.md § Web (emdawnwebgpu constraints).
        std::string s;
        s += "<!doctype html>\n";
        s += "<html lang=\"en\">\n";
        s += "<head>\n";
        s += "<meta charset=\"utf-8\">\n";
        s += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
        s += "<title>Context web build</title>\n";
        s += "<!-- Context web export shell (M8 a11). Boots the Emscripten/emdawnwebgpu RuntimeKernel\n";
        s += "     over the v1 chunked pack streamed by HTTP range requests. WebGPU-only (L-56). -->\n";
        s += "<style>html,body{margin:0;height:100%;background:#101014;color:#e0e0e6;"
             "font-family:system-ui,sans-serif}#context-canvas{display:block;width:100%;height:100%}"
             "</style>\n";
        s += "</head>\n";
        s += "<body>\n";
        s += "<canvas id=\"context-canvas\" tabindex=\"-1\"></canvas>\n";
        s += "<script>\n";
        s += "  // Emscripten Module config: the present canvas + the page-relative pack argument. The\n";
        s += "  // runtime streams content/<pack> in HTTP range chunks (see the a11 web pack streamer).\n";
        s += "  var Module = {\n";
        s += "    canvas: document.getElementById('context-canvas'),\n";
        s += "    arguments: ['--pack', 'content/" + plan.pack_name + "'],\n";
        s += "  };\n";
        s += "</script>\n";
        s += "<script src=\"bin/" + plan.runtime_loader + "\"></script>\n";
        s += "</body>\n";
        s += "</html>\n";
        return s;
    }
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
    // The web target's Emscripten JS glue (a11) — bin/context-runtime.js. Present only for web; the
    // native linux/windows runtimes are a single binary and omit it.
    if (!plan.runtime_loader.empty())
        s += "  \"runtimeLoader\": \"bin/" + json_escape(plan.runtime_loader) + "\",\n";
    s += "  \"pack\": \"content/" + json_escape(plan.pack_name) + "\",\n";
    s += "  \"launcher\": \"" + json_escape(plan.launcher_name) + "\",\n";
    // Whether the shipped runtime binary requires code-signing to ship (Windows Authenticode / macOS
    // Developer ID + notarization, R-SEC-003). The launcher + manifest are unsigned text either way;
    // only the runtime binary is signed.
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
