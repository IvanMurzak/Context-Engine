// The LIVE THIRD-PARTY PANEL smoke (M9 e13a-2) — ctest `editor-cef-smoke-shell-iframe`.
//
// It boots the SAME live windowless browser the sibling smokes boot, gives editor-core a roster
// carrying an `iframe` panel, and then asserts the ONE thing no other tier in this repo can reach:
// that a third-party package's document REALLY LOADED AND RAN over the `context-ext://` scheme,
// inside a sandboxed frame, under the strict response CSP.
//
// WHY THE OBSERVABLE IS "WHICH URLS WERE SERVED" AND NOT A FRAME HANDLE OR A PIXEL. A package panel
// lives on its own OPAQUE origin: the Shell cannot read its DOM, editor-core cannot read it either
// (that is the entire point of the sandbox), and e13a-2 deliberately ships no bridge into it. So the
// only honest evidence is the bytes the scheme handler actually delivered — `ext_served_urls()` /
// `ext_refused_urls()` (cef_shell.h). What makes that evidence STRONG rather than incidental is the
// FIXTURE, which is built so that each URL can only be requested if a specific earlier thing worked:
//
//   index.html  the frame NAVIGATED to the package origin. (Weak on its own — a frame blocked by
//               `frame-ancestors` still fetches its document; the response is where CSP is read.)
//   panel.css   the document was PARSED. A blocked frame's never is. This is also what proves
//               `style-src 'self'` matches inside an OPAQUE-origin document.
//   panel.js    `script-src 'self'` matched for the module ENTRY.
//   ready.js    the module GRAPH resolved — panel.js's own static `import`. This is the one that
//               dies silently without the CORS header ext_scheme.h documents.
//   ran.js      the module BODY EXECUTED — a dynamic `import()` at evaluation time. Scripts really
//               run in a frame whose only sandbox token is `allow-scripts`.
//
// AND THE NEGATIVE HALF, which is what keeps the positive from being vacuous: a SECOND iframe panel
// naming a package that is NOT mounted must be REFUSED. editor-core mounts it (its entry is a
// well-formed `context-ext://` URL, so refusing it is not editor-core's job), the browser requests
// it, and the deny-by-default resolver answers 404. Without this, a smoke could pass with a resolver
// that served everything to everyone.
//
// WHAT THIS SMOKE DOES NOT CLAIM (design 09 §3, honesty):
//   * It does not prove per-extension PROCESS isolation. That rides Chromium's
//     `IsolateSandboxedIframes` feature default (B-F6) and is not observable from here.
//   * On the Session-0 Windows runner it proves the same chain the Linux leg does — every assertion
//     is about bytes over the scheme and a composited software-OSR frame, none about a real window,
//     a GPU present, or user input. Nothing here is weaker on Windows and nothing here is claimed
//     to cover interactive behaviour.
//   * It says nothing about the panel BRIDGE: there is none yet (e13b), and this smoke asserts
//     `bridge.refused() == 0` precisely so an accidental early transport would be caught.
//
// TEST-ONLY MOUNT. `CefShellOptions::ext_packages` has no producer in the tree (the install flow is
// e13b's), so this smoke IS the first caller: it writes a fixture package into a temp dir and mounts
// it. That is the sanctioned way to reach the boundary end to end without building an install flow
// e13b owns, and it weakens nothing — the resolver's rules are unchanged and the fixture is just a
// directory.
//
// It boots exactly as cef_shell_settings_smoke.cpp does — a windowless browser presenting through
// e03's MemoryBlitter, hard-exiting on Windows after the verdict to skip CEF's flaky Session-0
// teardown — so it is structurally identical to that proven-green single-boot smoke, and it can only
// run where CEF links: the per-OS `editor-cef-smoke` CI job (Windows/Linux; macOS's .app packaging
// is e12's).

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/shell/app_scheme.h"
#include "context/editor/shell/banners.h"
#include "context/editor/shell/cef/cef_shell.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/ext_scheme.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/keybindings_bridge.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/session_bridge.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/user_config.h"
#include "context/editor/shell/welcome.h"
#include "context/editor/shell/window_bridge.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace shell = context::editor::shell;
namespace gc = context::editor::gui::contract;
namespace render = context::render;
namespace present = context::render::present;

namespace
{

int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (condition)
    {
        return;
    }
    std::fprintf(stderr, "[editor-cef-smoke-shell-iframe] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define SMOKE_CHECK(cond, what) check((cond), (what), __LINE__)

// Flushed progress trace — the only failure signal for a hang inside the live CEF pump is a stalled
// heartbeat, and CEF does not link on the GCC dev host so this is not locally reproducible.
void trace(const char* msg)
{
    std::fprintf(stderr, "[editor-cef-smoke-shell-iframe] %s\n", msg);
    std::fflush(stderr);
}

std::uint64_t now_us()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// editor-core's built asset root, compiled in by CMake (cef/CMakeLists.txt). Empty fallback (not an
// #error) so the pre-push audit's check 9 can compile this TU standalone against the pinned CEF
// headers; the runtime guard below fails the smoke loudly if an empty root ever reaches it.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif

// The fixture package that IS mounted, and the one that deliberately is NOT.
//
// Both ids are valid under `is_valid_package_id` — that is the point of the second one. A refusal
// caused by a MALFORMED id would prove nothing about the mount table, which is the gate under test
// here; the unmounted package must fail on being unmounted and on nothing else.
constexpr const char* kMountedPackage = "ctx.smoke-panel";
constexpr const char* kUnmountedPackage = "ctx.smoke-absent";

std::string ext_url(const char* package, const char* path)
{
    return std::string(shell::kExtUrlPrefix) + package + "/" + path;
}

bool contains(const std::vector<std::string>& haystack, const std::string& needle)
{
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

void dump(const char* label, const std::vector<std::string>& urls)
{
    std::fprintf(stderr, "[editor-cef-smoke-shell-iframe] %s (%zu):\n", label, urls.size());
    for (const std::string& url : urls)
    {
        std::fprintf(stderr, "    %s\n", url.c_str());
    }
    std::fflush(stderr);
}

bool write_file(const std::filesystem::path& path, const std::string& contents)
{
    // C++ streams, never C stdio: MSVC `/W4 /WX` rejects fopen/fwrite as C4996 and this TU is
    // compiled on the Windows CEF leg (conventions.md § no raw C stdio).
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out << contents;
    return out.good();
}

// THE FIXTURE PACKAGE. Every file exists to make one specific inference possible — see the header.
//
// Note what is NOT in index.html: no inline `<script>`, no `style="…"`, no external host. The panel
// CSP (`default-src 'none'`, no `'unsafe-inline'`) would refuse all three, so a fixture carrying one
// would fail for its own reasons and tell us nothing about the boundary.
bool write_fixture_package(const std::filesystem::path& root)
{
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec)
    {
        return false;
    }
    const bool ok =
        write_file(root / "index.html",
                   "<!doctype html>\n"
                   "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                   "<title>context smoke panel</title>\n"
                   "<link rel=\"stylesheet\" href=\"panel.css\">\n"
                   "<script type=\"module\" src=\"panel.js\"></script>\n"
                   "</head><body><main id=\"smoke-panel\">context smoke panel</main></body></html>\n") &&
        write_file(root / "panel.css", "#smoke-panel { color: rgb(18, 52, 86); }\n") &&
        // A STATIC import (fetched when the module GRAPH is built) and a DYNAMIC one (issued only if
        // the module BODY evaluated). Two different facts, deliberately kept in two different files.
        write_file(root / "panel.js",
                   "import \"./ready.js\";\n"
                   "void import(\"./ran.js\");\n") &&
        write_file(root / "ready.js", "export const ready = true;\n") &&
        write_file(root / "ran.js", "export const ran = true;\n");
    return ok;
}

// One `iframe` contribution for the roster (manifest v2, 04 §3/§5). Built here rather than added to
// `builtin_contributions()` on purpose: the built-in roster is gated by `gui-a11y-coverage` and the
// help/coverage manifests, and a smoke fixture has no business in the shipped panel set. PanelHost's
// explicit roster constructor exists for exactly this.
gc::Contribution iframe_contribution(const char* id, const std::string& entry)
{
    gc::Contribution c;
    c.id = id;
    c.kind = gc::ContributionKind::panel;
    c.title = id;
    c.dock.default_zone = gc::DockZone::right;
    c.dock.singleton = true;
    c.content.type = gc::ContentType::iframe;
    c.content.entry = entry;
    c.state.schema_version = 1;
    // The read/query baseline. e13a-2 grants nothing — the capability model is e13b's, and a
    // contribution that asked for more here would be asking a system that cannot yet answer.
    c.capabilities = {gc::kCapabilityReadQuery};
    return c;
}

int finish(int code)
{
#if defined(_WIN32)
    // Session-0 carve-out (mirrors cef_shell_smoke.cpp): CEF's teardown is flaky on the self-hosted
    // Windows runner, so exit hard once the verdict is decided.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
#else
    return code;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    // Subprocess re-entry FIRST: CEF's renderer/GPU/utility processes re-exec this binary.
    const int subprocess_exit = shell::cef::execute_subprocess(argc, argv);
    if (subprocess_exit >= 0)
    {
        return subprocess_exit;
    }

    std::printf("[editor-cef-smoke-shell-iframe] live third-party package panel over the real CEF "
                "pump + the context-ext:// scheme\n");
    std::fflush(stdout);

    const std::filesystem::path asset_root = CONTEXT_WEBUI_ASSET_DIR;
    if (asset_root.empty())
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-iframe] FAIL: CONTEXT_WEBUI_ASSET_DIR was not "
                             "compiled in (the webui asset root is unwired)\n");
        return finish(1);
    }

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-iframe-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);

    const std::filesystem::path package_root = project / "packages" / kMountedPackage;
    if (!write_fixture_package(package_root))
    {
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-iframe] FAIL: could not stage the fixture package at "
                     "%s\n",
                     package_root.string().c_str());
        return finish(1);
    }

    const render::Extent2D size{640, 480};

    shell::WindowDesc desc;
    desc.title = "Context Editor (iframe smoke)";
    desc.logical_size = size;
    desc.visible = false;
    auto backend = std::make_unique<shell::HeadlessWindowBackend>(desc);

    // --- the privileged bridges (the sibling smokes' set) -----------------------------------------
    // handshake BEFORE bridge (outlives the router that captures it); manager BEFORE the editor-state
    // bridge (whose sink captures &manager) — the declaration discipline cef_shell_smoke.cpp documents.
    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    shell::BridgeRouter bridge;

    // THE ROSTER UNDER TEST: every built-in, PLUS two package panels. The second names a package
    // that is never mounted, which is the smoke's negative half.
    std::vector<gc::Contribution> roster = gc::builtin_contributions();
    const std::size_t builtin_count = roster.size();
    roster.push_back(iframe_contribution(kMountedPackage, ext_url(kMountedPackage, "index.html")));
    roster.push_back(
        iframe_contribution(kUnmountedPackage, ext_url(kUnmountedPackage, "index.html")));

    shell::PanelHost panel_host(std::move(roster));
    SMOKE_CHECK(panel_host.roster_size() == builtin_count + 2,
                "the roster carries every built-in plus the two package panels");
    shell::panels::BuiltinPanels builtin = shell::panels::install_builtin_panels(panel_host);
    SMOKE_CHECK(builtin.bound == shell::panels::hostable_panel_ids().size(),
                "every hostable built-in panel provider bound (an extended roster changes nothing "
                "for them — the provider seam is keyed by id)");
    // NEITHER package panel is `hosted`, and that is CORRECT, not a gap: an iframe panel has no C++
    // provider by construction — its bytes come over the scheme, not over `panel.render`. This is
    // the assertion that pins editor-core's matching decision (PanelHost `#mountable`): if `hosted`
    // were ever true here, the renderer's iframe branch would be untested by this smoke.
    SMOKE_CHECK(!panel_host.hosts(kMountedPackage) && !panel_host.hosts(kUnmountedPackage),
                "a package panel is NOT `hosted` — it has no Shell provider by construction");
    SMOKE_CHECK(handshake.install(bridge), "the bridge handshake installed");
    SMOKE_CHECK(panel_host.install(bridge), "the panel.* bridge surface installed");

    shell::cef::CefShellOptions cef_options;
    cef_options.native_window = nullptr; // windowless: no native window on a Session-0 runner
    cef_options.logical_size = size;
    cef_options.dpi = shell::DpiScale{};
    cef_options.url = shell::kAppEntryUrl;
    cef_options.app_asset_root = asset_root;
    cef_options.bridge = &bridge;
    cef_options.windowless_frame_rate = 10;
    // Full verbose CEF logging so a refused subresource names its own cause on stderr — the shared
    // client's OnConsoleMessage reports the CSP directive by name, which is what turns a blank panel
    // from an archaeology exercise into a one-line diagnosis (DoD: a live smoke reports a CAUSE).
    cef_options.verbose_logging = true;
    // THE TEST-ONLY MOUNT. One package, its own disjoint root. The second package is deliberately
    // absent from this list.
    cef_options.ext_packages = {shell::ExtPackageMount{kMountedPackage, package_root}};

    std::printf("[editor-cef-smoke-shell-iframe] serving %s from %s; package %s mounted at %s\n",
                cef_options.url.c_str(), cef_options.app_asset_root.string().c_str(),
                kMountedPackage, package_root.string().c_str());
    std::fflush(stdout);

    std::string error;
    std::unique_ptr<shell::IBrowserHost> browser =
        shell::cef::make_cef_browser_host(cef_options, error);
    if (browser == nullptr)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-iframe] FAIL: the browser did not start: %s\n",
                     error.c_str());
        return finish(1);
    }
    trace("browser started (CefInitialize + CreateBrowserSync OK)");

    shell::EditorWindowConfig config;
    config.compositor.import_options.force_software = true; // software OSR — the shipping Windows path
    config.placement_poll_us = 0;
    auto window =
        std::make_unique<shell::EditorWindow>(std::move(backend), std::move(browser), config);

    auto blitter = std::make_unique<present::MemoryBlitter>();
    present::MemoryBlitter* blitter_raw = blitter.get();
    window->compositor().attach_cpu(std::move(blitter), size);

    shell::WindowManager manager(project);
    manager.add(std::move(window));
    shell::EditorWindow* editor = manager.window(0);
    SMOKE_CHECK(editor != nullptr, "the manager adopted the window");
    if (editor == nullptr)
    {
        return finish(1);
    }

    // --- the rest of the boot surface editor-core calls (identical to the sibling smokes) ---------
    // Each is called during boot; without them the router's deny-by-default `unknown_method` refusal
    // would trip the strict `bridge.refused() == 0` invariant below.
    shell::EditorStateBridge editor_state_bridge;
    editor_state_bridge.bind_store(&manager.state_store(), now_us);
    editor_state_bridge.bind_regions(
        [&manager](std::vector<shell::ShellRegion> regions)
        {
            if (shell::EditorWindow* target_window = manager.window(0))
            {
                target_window->input().regions().publish(std::move(regions));
            }
        });
    SMOKE_CHECK(editor_state_bridge.install(bridge),
                "the editor.state.*/editor.regions.* bridge surface installed");

    shell::KeybindingsBridge keybindings_bridge;
    keybindings_bridge.bind_path(std::filesystem::path{});
    SMOKE_CHECK(keybindings_bridge.install(bridge), "the keybindings.get bridge surface installed");

    shell::ThemesBridge themes_bridge;
    themes_bridge.bind_directory(std::filesystem::path{});
    SMOKE_CHECK(themes_bridge.install(bridge), "the themes.get bridge surface installed");

    // Installed UNBOUND (no path): editor-core reads the config at boot, and an uninstalled surface
    // would be an `unknown_method` refusal that trips `bridge.refused() == 0`. This smoke changes no
    // setting, so nothing is written and the runner's own config is never touched.
    shell::UserConfigStore user_config;
    SMOKE_CHECK(user_config.install(bridge), "the config.* bridge surface installed");

    shell::WelcomeBridge welcome_bridge;
    welcome_bridge.set_launch_mode(shell::LaunchMode::project);
    welcome_bridge.set_config_path(std::filesystem::path{});
    SMOKE_CHECK(welcome_bridge.install(bridge), "the welcome.state bridge surface installed");

    shell::BannerBridge banner_bridge;
    SMOKE_CHECK(banner_bridge.install(bridge), "the banner bridge surface installed");

    shell::SessionBridge session_bridge;
    SMOKE_CHECK(session_bridge.install(bridge), "the session.state bridge surface installed");

    shell::WindowMoveStore window_move_store;
    shell::WindowBridge window_move_bridge(shell::kPrimaryWindowId, window_move_store);
    SMOKE_CHECK(window_move_bridge.install(bridge), "the window.* bridge surface installed");

    // The five fixture URLs, in the order the chain of inference walks them.
    const std::string url_index = ext_url(kMountedPackage, "index.html");
    const std::string url_css = ext_url(kMountedPackage, "panel.css");
    const std::string url_module = ext_url(kMountedPackage, "panel.js");
    const std::string url_ready = ext_url(kMountedPackage, "ready.js");
    const std::string url_ran = ext_url(kMountedPackage, "ran.js");
    const std::string url_absent = ext_url(kUnmountedPackage, "index.html");

    // Drive the pump until the whole chain is observable. The deadline BOUNDS it: a panel that never
    // loads runs the clock out and the assertions below fail with the served list printed, rather
    // than hanging.
    const auto loop_start = std::chrono::steady_clock::now();
    const auto deadline = loop_start + std::chrono::seconds(45);
    bool presented = false;
    bool traced_presented = false;
    bool traced_handshake = false;
    bool traced_document = false;
    bool traced_module = false;
    auto last_heartbeat = loop_start;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (!presented && editor->compositor().stats().view_frames > 0 &&
            blitter_raw->blit_count() > 0)
        {
            presented = true;
        }
        if (presented && !traced_presented)
        {
            traced_presented = true;
            trace("milestone: first CEF OSR frame composited + presented");
        }
        if (!traced_handshake && handshake.complete())
        {
            traced_handshake = true;
            trace("milestone: bridge handshake complete");
        }
        const std::vector<std::string> served = shell::cef::ext_served_urls();
        if (!traced_document && contains(served, url_index))
        {
            traced_document = true;
            trace("milestone: the package's index.html was served over context-ext://");
        }
        if (!traced_module && contains(served, url_ready))
        {
            traced_module = true;
            trace("milestone: the panel's module GRAPH resolved (its own static import was served)");
        }
        if (presented && handshake.complete() && contains(served, url_ran) &&
            !shell::cef::ext_refused_urls().empty())
        {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat >= std::chrono::seconds(2))
        {
            last_heartbeat = now;
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - loop_start).count();
            std::fprintf(stderr,
                         "[editor-cef-smoke-shell-iframe] heartbeat t=%lldms presented=%d "
                         "handshake=%d lists=%llu ext_served=%zu ext_refused=%zu refusals=%llu\n",
                         static_cast<long long>(elapsed_ms), presented ? 1 : 0,
                         handshake.complete() ? 1 : 0,
                         static_cast<unsigned long long>(panel_host.lists_served()), served.size(),
                         shell::cef::ext_refused_urls().size(),
                         static_cast<unsigned long long>(bridge.refused()));
            std::fflush(stderr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // --- the boot invariants (the app is genuinely up — same as the sibling smokes) ---------------
    SMOKE_CHECK(presented, "a real CEF OSR frame was composited and presented within the deadline");
    SMOKE_CHECK(handshake.complete(),
                "the IPC bridge round-tripped native<->JS: editor-core echoed the handshake nonce");
    SMOKE_CHECK(panel_host.lists_served() >= 1,
                "editor-core READ the roster (panel.list) — without it nothing could have decided to "
                "mount a package panel at all");

    manager.shutdown();
    shell::cef::shutdown();

    // Read the log AFTER shutdown: the CE #319 lifetime invariant forces `shutdown()` before the
    // bridge locals die, and the log deliberately outlives it (cef_shell.cpp) so the verdict below
    // sees every request, including any that completed during teardown.
    const std::vector<std::string> served = shell::cef::ext_served_urls();
    const std::vector<std::string> refused = shell::cef::ext_refused_urls();
    dump("context-ext:// SERVED", served);
    dump("context-ext:// REFUSED", refused);

    // --- THE CHAIN. Each step is asserted separately so a red leg names WHERE it broke ------------
    SMOKE_CHECK(contains(served, url_index),
                "the sandboxed frame NAVIGATED to the package origin and its document was served "
                "over context-ext:// (if this is the only failure, editor-core never created the "
                "frame — look at the roster/manifest, not at the scheme)");
    SMOKE_CHECK(contains(served, url_css),
                "the package DOCUMENT WAS PARSED — it requested its own stylesheet. A frame refused "
                "by `frame-ancestors` fetches its document and then parses nothing, so this is the "
                "assertion that verifies the e13a-2 host-source tightening, and it also proves "
                "`style-src 'self'` matches inside an OPAQUE-origin (sandboxed) document");
    SMOKE_CHECK(contains(served, url_module),
                "`script-src 'self'` matched the panel's own module ENTRY");
    SMOKE_CHECK(contains(served, url_ready),
                "the module GRAPH resolved (panel.js's own static import). THIS is the one that dies "
                "silently without `Access-Control-Allow-Origin: null` on script responses — a module "
                "is fetched in CORS mode and a sandboxed frame's origin is the opaque `null`; see "
                "ext_scheme.h for the measurement");
    SMOKE_CHECK(contains(served, url_ran),
                "the module BODY EXECUTED (its dynamic import fired) — scripts really do run in a "
                "frame whose only sandbox token is `allow-scripts`");

    // --- the negative half: deny-by-default is LIVE in the browser, not just in the unit suite ----
    SMOKE_CHECK(contains(refused, url_absent),
                "the UNMOUNTED package was REFUSED. Both fixture ids are grammatically valid, so "
                "this refusal can only come from the mount table — which is what makes the served "
                "list above evidence rather than a resolver that serves everything");
    SMOKE_CHECK(std::none_of(served.begin(), served.end(),
                             [](const std::string& url)
                             { return url.find(kUnmountedPackage) != std::string::npos; }),
                "NOTHING from the unmounted package was ever served");
    SMOKE_CHECK(std::none_of(refused.begin(), refused.end(),
                             [](const std::string& url)
                             { return url.find(kMountedPackage) != std::string::npos; }),
                "and nothing from the MOUNTED package was refused — a partially-served panel would "
                "make every positive above ambiguous");

    // --- no bridge reached the panel (e13a-2 ships the host, not the transport) -------------------
    SMOKE_CHECK(bridge.refused() == 0,
                "the live scenario produced no envelope refusals (a refusal here would mean either a "
                "boot surface is missing or something tried to speak a panel-bridge protocol that "
                "does not exist yet)");
    SMOKE_CHECK(bridge.secrets_blocked() == 0,
                "no handler attempted to return a protected credential during the scenario");
    SMOKE_CHECK(shell::cef::popups_suppressed() == 0 && shell::cef::browsers_created() == 1,
                "exactly one browser, and the package panel opened no popup");

    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-iframe] FAILED with %d assertion failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-iframe] PASS: a third-party package panel loaded, parsed, "
                "and RAN inside a sandboxed context-ext:// frame under the strict panel CSP, while "
                "an unmounted package stayed refused\n");
    return finish(0);
}
