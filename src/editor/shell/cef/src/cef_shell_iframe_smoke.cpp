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
// M9 e13b-1 EXTENDS THE SAME TECHNIQUE TO THE PORT, which is the only tier that can prove it: a panel
// port is a `MessagePort` inside an opaque-origin document, so nothing native or TS-side can observe
// one from outside. Four more URLs, each behind a branch a specific port fact had to satisfy:
//
//   .context-panel-port.js  the SHELL SPLICED its bootstrap tag into the package's document bytes and
//               the browser fetched it — from the package's own origin, out of the scheme itself, with
//               no such file on disk. (The one link with no unit-tier equivalent at all.)
//   port-present.js         the bootstrap RAN, and ran BEFORE the package's own module: the module
//               found `window.contextPanelPort` already published. That ordering is what the one-shot
//               rests on.
//   port-refused.js         THE ROUND TRIP. The panel transferred a port up, editor-core accepted it,
//               the panel sent a STILL-PARKED verb over it, and the reply matched the deny-all refusal
//               exactly — tag, version, id, `ok:false`, `bridge.verb_not_granted`, echoed verb.
//               ⚠ THE SUBJECT VERB MOVES AS VERBS ARE FILLED, and re-pointing it is maintenance, not
//               a weakening. It named `bridge.call` until M9 e13c-1 FILLED that verb, at which point
//               this step asserted the opposite of the truth and this smoke went red on two legs.
//               `bridge.events.subscribe` is the verb genuinely still parked (e13c-2's — it needs a
//               BOUNDED fan-out buffer with an ack cursor). Re-point at the next parked verb whenever
//               one is filled; the property under test — that filling SOME entries did not open the
//               whole table — is what must survive. The T1 tier keeps the same step in step
//               (panelport.test.ts, "A STILL-PARKED verb keeps the e13b-1 answer").
//   one-shot-held.js        the panel offered a SECOND channel and, after a settle window, had
//               received nothing on it. The failure direction is SAFE: a second port that WAS granted
//               sets the fixture's flag and this import never fires.
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
//   * It says nothing about which PACKAGE the document holding the port belongs to. The port is bound
//     to the FIRST document loaded into the frame (ext_scheme.h § the E13B obligation); that this was
//     the package the host asked for is a Shell/editor-core agreement, not something the browser
//     attests, and no assertion here upgrades it.
//   * `port-refused.js` proves a TRANSPORT and an AUTHENTICATION boundary — never a granted
//     capability. It is asserted with a verb the table does not carry, so what it measures is that
//     an ungranted verb travels and comes back refused INTACT. The port itself is not capability-free
//     (the table has carried `bridge.commands.*`, `bridge.ui.subscribe`, `bridge.theme.tokens` and
//     `bridge.state.get|set` since before e13c-1, which added `bridge.call`); no assertion here says
//     anything about what a GRANTED verb does — that is the T1 tier's and the C++ session tier's.
//   * `bridge.refused() == 0` is still asserted, and still says nothing about the port: the panel
//     transport rides MessageChannel/postMessage and never reaches the CEF message router.
//
// TEST-OWNED STORE, no longer a test-only SEAM. When this smoke was written `CefShellOptions::
// ext_packages` had no producer at all and this was its first caller. M9 e13c-3 gave it a real one
// (`editor_main.cpp`, out of `~/.context/packages`) and made `mount()` check every root's PROVENANCE
// against a store root — so what this smoke does now is stage a fixture package inside its OWN temp
// package store and name that store, which is byte-for-byte the shape production takes. It is
// therefore no longer a weaker path than the real one: the same six provenance refusals apply to this
// fixture as to an installed package, and the fixture passes them because it genuinely is a real
// directory one level under a real store root. (Compare the settings smoke, which binds a TEMP
// `config.json` rather than the runner's own for exactly the same reason.)
//
// It boots exactly as cef_shell_settings_smoke.cpp does — including the TWO WINDOW MODES documented
// in cef_shell_smoke.cpp's header: windowless through e03's MemoryBlitter by DEFAULT (what the
// Windows leg runs), a REAL X11 window through the REAL X11 blitter under `--real-window` (what the
// ctest registration passes on Linux). It hard-exits on Windows after the verdict to skip CEF's
// flaky Session-0 teardown — so it is structurally identical to that proven-green smoke, and can only
// run where CEF links: the `editor-cef-smoke` CI job, on all three OSes since M9 e12c-2 (macOS boots
// it from a real `.app` carrying its five per-process-type helper bundles).

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
#include "context/editor/shell/smoke/smoke_window.h"
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
namespace smoke = context::editor::shell::smoke;
namespace render = context::render;

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
    // The M9 e13b-1 PORT LEG of the fixture. Each dynamic import below is issued ONLY from inside a
    // branch that a specific port fact has already satisfied, so its URL appearing in
    // `ext_served_urls()` IS the assertion — the same inference the css/ready/ran chain above uses,
    // extended to the transport.
    //
    // ⚠ THE WIRE STRINGS ARE SPELLED HERE ON PURPOSE, and only the handshake tag is taken from a
    // constant (`kExtPortHandshakeTag`, which the Shell itself emits). `context.panel-bridge`, the
    // envelope version and `bridge.verb_not_granted` are editor-core's (panelport.ts
    // `PANEL_BRIDGE_TAG` / `PANEL_BRIDGE_VERSION` / `PANEL_BRIDGE_REFUSALS`), and a test that
    // referenced them through a shared constant would be asserting that the two sides agree with
    // themselves. A CONTRACT test must restate the contract: if editor-core renames a refusal code,
    // this smoke must go red, because a shipped panel written against the old spelling would too.
    const std::string port_js =
        std::string("var port = window.contextPanelPort;\n"
                    "if (port) {\n"
                    // The bootstrap ran BEFORE this module — a classic external script blocks
                    // parsing, a module is deferred — so the port is already published.
                    "  void import(\"./port-present.js\");\n"
                    "  port.onmessage = function (event) {\n"
                    "    var d = event.data;\n"
                    "    if (d && d.ctx === \"context.panel-bridge\" && d.v === 1 &&\n"
                    "        d.kind === \"reply\" && d.id === \"smoke-1\" && d.ok === false &&\n"
                    "        d.error && d.error.code === \"bridge.verb_not_granted\" &&\n"
                    "        d.error.verb === \"bridge.events.subscribe\") {\n"
                    "      void import(\"./port-refused.js\");\n"
                    "    }\n"
                    "  };\n"
                    "  port.start();\n"
                    "  port.postMessage({ ctx: \"context.panel-bridge\", v: 1, kind: \"request\",\n"
                    "                     id: \"smoke-1\", verb: \"bridge.events.subscribe\" });\n"
                    // THE ONE-SHOT, LIVE. A second handshake must win nothing. The child cannot
                    // observe a refusal directly (the host just closes the port it was handed), so it
                    // observes SILENCE on the second channel after the first channel has demonstrably
                    // been serviced — and the failure direction is SAFE: a second port that WAS granted
                    // sets `secondSaw`, the import never fires, and the smoke goes red.
                    "  var second = new MessageChannel();\n"
                    "  var secondSaw = false;\n"
                    "  second.port1.onmessage = function () { secondSaw = true; };\n"
                    "  second.port1.start();\n"
                    "  parent.postMessage({ ctx: \"") +
        shell::kExtPortHandshakeTag +
        "\" }, \"*\", [second.port2]);\n"
        "  second.port1.postMessage({ ctx: \"context.panel-bridge\", v: 1, kind: \"request\",\n"
        "                             id: \"smoke-2\", verb: \"bridge.events.subscribe\" });\n"
        "  setTimeout(function () {\n"
        "    if (!secondSaw) { void import(\"./one-shot-held.js\"); }\n"
        "  }, 1500);\n"
        "}\n";

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
                   "void import(\"./ran.js\");\n" +
                       port_js) &&
        write_file(root / "ready.js", "export const ready = true;\n") &&
        write_file(root / "ran.js", "export const ran = true;\n") &&
        write_file(root / "port-present.js", "export const present = true;\n") &&
        write_file(root / "port-refused.js", "export const refused = true;\n") &&
        write_file(root / "one-shot-held.js", "export const held = true;\n");
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

    // e12a-x11-legs: `--real-window` (passed by the ctest registration on Linux) runs this whole
    // scenario over a REAL X11 window + the REAL X11 present blitter, instead of the offscreen
    // shell. Parsed AFTER the subprocess re-entry above: a CEF renderer/GPU child inherits the flag
    // on its command line and must never reach this body.
    const smoke::WindowMode window_mode = smoke::window_mode_from_args(argc, argv);

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

    // `<project>/packages` IS this smoke's package store (M9 e13c-3) — the fixture already had the
    // shape, so naming it is the whole migration: the store root is what `mount()` now checks every
    // root's PROVENANCE against, and a fixture package one level under it passes for the same reason a
    // really-installed one does. Kept as a temp dir rather than pointed at the runner's real
    // `~/.context/packages`, exactly as the settings smoke binds a TEMP config file rather than the
    // runner's own `~/.context/config.json`.
    const std::filesystem::path package_store = project / "packages";
    const std::filesystem::path package_root = package_store / kMountedPackage;
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
    smoke::WindowSetup window_setup = smoke::make_smoke_window(desc, window_mode);
    if (window_setup.backend == nullptr)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-iframe] FAIL: no %s window could be created: %s\n",
                     smoke::to_string(window_mode), window_setup.diagnostic.c_str());
        return finish(1);
    }
    // The size the WINDOW actually got: a real display's DPI makes it differ from the request, and
    // CEF's view rect is DIP, so telling the browser the request would lay the document out wrong.
    const smoke::BrowserGeometry geometry = smoke::browser_geometry(*window_setup.backend);

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
    // Windowless in BOTH window modes, and not merely for Session 0: cef_shell.cpp reads
    // `native_window` only under _WIN32, so on Linux CEF stays windowless-OSR either way and the
    // Shell's own X11 window is purely the PRESENT target.
    cef_options.native_window = nullptr;
    cef_options.logical_size = geometry.logical_size;
    cef_options.dpi = geometry.dpi;
    cef_options.url = shell::kAppEntryUrl;
    cef_options.app_asset_root = asset_root;
    cef_options.bridge = &bridge;
    cef_options.windowless_frame_rate = 10;
    // Isolate the OSCrypt profile-encryption key from the MACHINE keychain (issue #437). Without
    // this, macOS blocks CefShutdown() forever on a SecurityAgent authorization prompt no automated
    // run can answer, so the smoke prints its whole verdict and then never exits — see
    // CefShellOptions::use_mock_keychain for the mechanism. EVERY CEF smoke sets it, and
    // tools/check_cef_keychain_isolation.py fails the build if one stops.
    cef_options.use_mock_keychain = true;
    // Full verbose CEF logging so a refused subresource names its own cause on stderr — the shared
    // client's OnConsoleMessage reports the CSP directive by name, which is what turns a blank panel
    // from an archaeology exercise into a one-line diagnosis (DoD: a live smoke reports a CAUSE).
    cef_options.verbose_logging = true;
    // THE TEST-ONLY MOUNT. One package, its own disjoint root, under the store above. The second
    // package is deliberately absent from this list — which since e13c-3 is a mount-table fact AND a
    // store fact: `kUnmountedPackage` has no directory in `package_store` either, so the refusal the
    // NEGATIVE half asserts is what a genuinely-uninstalled package produces.
    cef_options.ext_packages = {shell::ExtPackageMount{kMountedPackage, package_root}};
    cef_options.ext_store_root = package_store;

    std::printf("[editor-cef-smoke-shell-iframe] serving %s from %s; package %s mounted at %s "
                "(store %s)\n",
                cef_options.url.c_str(), cef_options.app_asset_root.string().c_str(),
                kMountedPackage, package_root.string().c_str(), package_store.string().c_str());
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
        std::make_unique<shell::EditorWindow>(std::move(window_setup.backend), std::move(browser), config);

    // e03's portable blitter offscreen; in real mode the REAL OS blitter that
    // `EditorWindow::attach_cpu_present()` selects from the REAL native window — the same call
    // `context_editor` makes on a GPU-less boot. Real mode REFUSES the in-memory blitter.
    const smoke::PresentSetup present_setup = smoke::attach_smoke_present(*window, window_mode);
    if (!present_setup.ok)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-iframe] FAIL: no %s present path: %s\n",
                     smoke::to_string(window_mode), present_setup.diagnostic.c_str());
        return finish(1);
    }

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
    // The M9 e13b-1 port chain. `url_bootstrap` is the SYNTHETIC asset the scheme serves out of itself
    // — its presence is what proves the Shell SPLICED the tag into the package's document and the
    // browser fetched it; the other three are ordinary package files each guarded by a port fact.
    const std::string url_bootstrap = ext_url(kMountedPackage, shell::kExtPortBootstrapAsset);
    const std::string url_port_present = ext_url(kMountedPackage, "port-present.js");
    const std::string url_port_refused = ext_url(kMountedPackage, "port-refused.js");
    const std::string url_one_shot = ext_url(kMountedPackage, "one-shot-held.js");

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
    bool traced_port = false;
    auto last_heartbeat = loop_start;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!manager.pump_once(now_us()))
        {
            break;
        }
        if (!presented && editor->compositor().stats().view_frames > 0 &&
            editor->compositor().stats().frames_presented > 0)
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
        // WAIT FOR THE EXACT FACT THE VERDICT ASSERTS, not for a weaker proxy of it. `refused` being
        // merely NON-EMPTY would also be satisfied by some other refusal landing first (a
        // not-found subresource, say), letting the loop break before the unmounted package's own
        // request has been answered — a red on a perfectly healthy build.
        if (!traced_port && contains(served, url_port_refused))
        {
            traced_port = true;
            trace("milestone: the panel's PORT round-tripped (it received the deny-all refusal)");
        }
        // The LAST fact to arrive is the one-shot verdict (the fixture settles for 1.5s before
        // deciding), so waiting on it waits on everything.
        if (presented && handshake.complete() && contains(served, url_ran) &&
            contains(served, url_one_shot) &&
            contains(shell::cef::ext_refused_urls(), url_absent))
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
                "by `frame-ancestors` fetches its document and then parses nothing, so this proves "
                "the directive ADMITTED the editor's frame — i.e. the e13a-2 host-source tightening "
                "did not silently collapse it — and it also proves `style-src 'self'` matches inside "
                "an OPAQUE-origin (sandboxed) document. NOTE the direction: this cannot catch a "
                "REVERT to the looser scheme-source form, which still admits context-editor://app "
                "and leaves every URL below served. The exact directive text is pinned by "
                "test_ext_scheme.cpp instead");
    SMOKE_CHECK(contains(served, url_module),
                "`script-src 'self'` matched the panel's own module ENTRY");
    SMOKE_CHECK(contains(served, url_ready),
                "the module GRAPH resolved (panel.js's own static import). THIS is the one that dies "
                "silently without `Access-Control-Allow-Origin: null` on script responses — a module "
                "is fetched in CORS mode and a sandboxed frame's origin is the opaque `null`; see "
                "ext_scheme.h for the measurement");
    SMOKE_CHECK(contains(served, url_ran),
                "the module BODY EXECUTED (its dynamic import fired) — scripts really do run in the "
                "frame editor-core created. This says nothing about the SANDBOX: dropping the "
                "attribute would give the frame its real origin, make the module fetches "
                "same-origin, and leave every URL here served just the same. The attribute's token "
                "set is pinned on the rendered element by the T1 tier (extpanel.test.ts)");

    // --- the M9 e13b-1 PORT CHAIN, each link its own assertion ------------------------------------
    SMOKE_CHECK(contains(served, url_bootstrap),
                "the SHELL-INJECTED PORT BOOTSTRAP was served. This is the one link no other tier can "
                "reach: `ext_inject_port_bootstrap` spliced a `<script src>` into the package's own "
                "document bytes, the browser resolved it against the package ORIGIN (an absolute path "
                "— a relative one would 404 for any entry not at the root), and the scheme answered it "
                "out of itself with no file on disk. If this is the only failure, look at the splice "
                "point or the synthetic-asset branch, not at the port");
    SMOKE_CHECK(contains(served, url_port_present),
                "the bootstrap RAN, and it ran BEFORE the package's own module: the module found "
                "`window.contextPanelPort` already published. A classic external script blocks parsing "
                "and a module is deferred, which is the ordering the whole one-shot rests on — if this "
                "fails while the bootstrap above was served, the script executed but published nothing");
    SMOKE_CHECK(contains(served, url_port_refused),
                "THE PORT ROUND-TRIPPED THROUGH THE REAL CEF PUMP. The panel transferred a port up, "
                "editor-core accepted it, the panel sent a request for a STILL-PARKED verb "
                "(`bridge.events.subscribe`) over it, and the reply it got back matched the deny-all "
                "refusal EXACTLY — tag, version, id, ok:false, `bridge.verb_not_granted`, and the "
                "echoed verb. This is the e13b-1 deliverable proven end to end across an "
                "opaque-origin boundary, and no unit tier can produce it. If a later task FILLS this "
                "verb, re-point the fixture at the next parked one (file header) — do not delete the "
                "step, and do not assert a granted verb here");
    SMOKE_CHECK(contains(served, url_one_shot),
                "and the grant is ONE-SHOT in the live browser: the panel offered a SECOND channel and "
                "sent a request over it, and after a settle window had received nothing at all. NOTE "
                "the failure direction is safe — if a second port HAD been granted, the fixture's flag "
                "is set and this import never fires, so the wrong outcome reddens rather than passing");

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

    // --- the panel transport does NOT ride the CEF router (e13b-1 ships a port, not a verb) -------
    SMOKE_CHECK(bridge.refused() == 0,
                "the live scenario produced no envelope refusals — every verb the boot path spoke "
                "was one the router knows. This is a boot-surface invariant, NOT a check on the "
                "panel transport: the e13b-1 port rides MessageChannel/postMessage, which never "
                "reaches this router, so the port chain asserted above neither adds to nor "
                "subtracts from this count");
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
