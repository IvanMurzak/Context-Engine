// The LIVE TWO-WINDOW DAEMON-BACKED INSPECTOR FAN-OUT smoke (M9 e09e-3) — ctest
// `editor-cef-smoke-shell-inspector-fanout`. THE KEYSTONE of e09: design 05 §8's canonical flow,
// end to end, against a REAL daemon, driven from a REAL DOM, observed in a SECOND live window.
//
// §8, verbatim, and where each link is proven here:
//
//   DOM input (window A) -> hydration -> command "inspector.edit"      <- §2 below, a REAL `input`
//     -> panel model stage_edit (C++ via bridge)                          event on the REAL element
//     -> gesture end -> commit -> WireOverrideWriteGateway              <- §3, a REAL `change` event
//     -> RPC edit {file, pointer, value, ifMatch}                       <- §3, the bytes ON DISK
//     -> daemon write queue -> atomic write -> watcher/hash -> derivation
//     -> events: files.changed -> derivation.settled{gen}               <- M9 x9's publisher
//     -> all subscribed clients (window 1, window 2, CLI, agents) update <- §4, window B's Inspector
//
// ⚠⚠ WHAT THIS SMOKE ADDS OVER ITS CEF-FREE SIBLING, precisely — because if the answer were
// "nothing" it would be a 700-line tautology. `editor-session-concurrent-cas-t2`
// (src/tests/integration/test_e09b_concurrent_cas.cpp § 5) already drives TWO `BuiltinPanels` bags
// against a REAL daemon and asserts the whole fan-out and its L-30 deferral. It cannot reach three
// things, and those three are all this file asserts that it does not:
//
//   1. **THE FIRST LINK IS A REAL DOM EVENT, not a `PanelHost::invoke` call.** The sibling calls
//      `host.invoke(...)` / `host.gesture(...)` directly — the C++ end of the bridge. Here the edit
//      begins as an `input` event dispatched on the `<input>` element the REAL hydration runtime
//      rendered from the REAL panel model, in a REAL browser: `#stageFrom` -> `commandValueFor` ->
//      `PanelClient.command(..., value)` -> the CEF message router -> `panel.command` -> the
//      provider. e09e-1 built that path and NOTHING has ever run it live. The proof it ran is that
//      window A's model holds a staged edit at all — a value no C++ in this file ever passed in.
//   2. **TWO LIVE editor-cores against ONE daemon.** This is the first `editor-cef-smoke-*` TU ever
//      to spawn a daemon (through the PRODUCTION `DaemonLifecycle`, not a hand-rolled child), and
//      the two windows are created by the REAL window registry, each booting its own editor-core
//      instance with its own bridge router and its own panel bag — the topology 02 §2 describes
//      (one process, N windows, one daemon lifecycle).
//   3. **WINDOW B'S RENDERED DOM IS READ BACK.** §5 dispatches an `input` event on window B's field
//      WITHOUT touching its value, so `commandValueFor` reads the value the element is DISPLAYING
//      and stages it. The value that arrives in window B's C++ model therefore came OUT of the
//      DOM — which is the only way to assert "window B's Inspector reflects it" about the surface a
//      human would actually look at, rather than about the model behind it.
//
// TWO KNOWN DEFECTS BOUND THIS SMOKE'S SHAPE. One is now FIXED elsewhere; the other is still open.
//
//   * **CE #452 — FIXED by M9 x10, but the avoidance below STAYS, for a different reason.** A staged
//     gesture used to be silently discarded when another client moved the SHARED selection (daemon
//     state since e08b). x10 closed both doors (`InspectorFeed::request` / `request_clear` now DEFER
//     any selection-driven read behind an in-flight gesture) and made a genuinely-undeferrable
//     abandonment LOUD through the write-notice relay. This smoke still NEVER moves the selection while
//     a gesture is staged — both windows are armed on the same identity up front (§1) and every later
//     step edits a VALUE, which `identity_hash_of` does not depend on — but the reason is now
//     STRUCTURAL rather than a bug-avoidance: this file binds BOTH windows to ONE daemon connection
//     (see the MIRROR IS NOT EXACT note below), so every `selection-changed` fact it could provoke
//     carries its OWN `origin` and is dropped by echo suppression BY DESIGN. A foreign selection move
//     needs two connections, which is **CE #455**. x10's proof therefore lives in the CEF-free sibling
//     `editor-session-concurrent-cas-t2` (§ 5e), which holds three separately-attached clients. Do not
//     "simplify" a selection move into §5 expecting it to exercise x10 — here it would be a self-echo.
//   * **CE #451** — the settle on `edit` is unbounded under `dispatch_mu`, a LOAD-only latency
//     exposure. Idle cost is nil and this smoke is idle, so it does not appear here; a mysterious
//     stall under a loaded runner would be #451, not this file.
//
// ⚠⚠ TWO PRODUCT DEFECTS THIS SMOKE FOUND, both fixed in the same PR, because they are §8's LAST
// LINK and this is the first test in the repo that could see it. Neither was visible to any other
// tier: the C++ suites drive the Shell side of the panel seam and never the hydration runtime, and
// the `webui-ts-unit` tier applies ONE revision per case, so the whole re-render path was untested.
//
//   * **THE PATCH WIPED THE PANEL.** `HydrationRuntime.apply` handed its incremental patcher the
//     `<template>` ELEMENT instead of `template.content`, and a template's own `children` is always
//     empty — so every patch read an EMPTY source and its trailing-removal loop deleted every child
//     of the panel body. It looked survivable because the wipe leaves the container empty, so the
//     NEXT render re-mounts: a panel that re-rendered TWICE healed itself, and only a panel that
//     re-rendered ONCE stayed blank. Here window B staged one DOM edit, the resulting refresh emptied
//     its Inspector, and with no field left to type into nothing could ever refresh it again.
//   * **NOTHING RE-RENDERED A PANEL WHEN ITS MODEL MOVED.** Every re-render was driven by LOCAL
//     interaction (mount, becoming visible, a command THIS window sent), so a fact arriving from the
//     DAEMON reached the C++ model and changed nothing on screen — §8's "all subscribed clients
//     (window 1, window 2, …) update" was false of the only surface a human looks at, and worst in a
//     secondary window, which sends no commands at all. `PanelHost.pollRevisions` (a revision-gated
//     poll on editor-core's existing tick) is the driver; the plant that proves this smoke depends on
//     it is removing that one call, which reds the smoke.
//
// WHAT THIS SMOKE DELIBERATELY DOES NOT CLAIM. It proves the chain for the panel bags this process
// FEEDS. Feeding EVERY window's bag from the one daemon link is a property of the composition root,
// and `editor_main.cpp` gained it in this same PR (its `SecondaryWindowSurfaces` used to be wired to
// nothing daemon-side, so §8's tail was unreachable for any window but the first). This file drives
// the same four per-window seams in the same order and pumps them per frame, so a regression that
// unwires THIS file's secondary fan-out reds here. Stated precisely because the difference matters:
// that is a MIRROR of the composition root, not a test OF it. `editor_main.cpp` has no test tier at
// all (it is a `main()`), so its own wiring is compile-checked and reviewed, and the honest claim is
// that the seams this file drives are the ones it calls.
//
// ⚠ AND THE MIRROR IS NOT EXACT IN ONE NAMED RESPECT — do not read the paragraph above as more than
// it says. `editor_main.cpp`'s factory gives every secondary window its OWN wire connection
// (`parts.daemon_client`, one `attach_to_project` per window) and binds/pumps that window's bag
// against it, so N windows are genuinely N daemon-minted `origin`s (window_registry.h: ids are minted
// per WIRE CONNECTION). This file sets no `parts.daemon_client` at all: `pump_once` binds BOTH bags to
// the single `lifecycle.client()`. So window B here reads and writes over window A's connection and
// shares its `origin`, and §6's L-30 race is single-connection where production is two. That does not
// weaken any assertion below — the `edit` CAS is keyed on the `ifMatch` RAW HASH, which no `origin`
// participates in, and the on-disk pair at the end is byte evidence either way — but the PER-WINDOW
// connection, the PER-WINDOW `origin`, and per-window echo suppression are NOT covered here. Their
// coverage is the CEF-free sibling named above, `editor-session-concurrent-cas-t2`, which attaches
// three SEPARATE clients and asserts their daemon-minted ids differ
// (test_e09b_concurrent_cas.cpp § `shell_client` / `racer` / `observer`); making this smoke
// cross-connection too is CE #455, not a property to assume from the word "mirror".
//
// TWO WINDOW MODES, as documented in cef_shell_smoke.cpp's header: headless by default (safe on the
// Session-0 Windows runner), REAL windows under `--real-window`, which the ctest registration passes
// on Linux. Both windows honour it.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "context/editor/client/client.h"
#include "context/editor/shell/app_scheme.h"
#include "context/editor/shell/banners.h"
#include "context/editor/shell/cef/cef_shell.h"
#include "context/editor/shell/daemon_lifecycle.h"
#include "context/editor/shell/editor_state_bridge.h"
#include "context/editor/shell/keybindings_bridge.h"
#include "context/editor/shell/package_grants.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/session_bridge.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/smoke/smoke_window.h"
#include "context/editor/shell/themes_bridge.h"
#include "context/editor/shell/user_config.h"
#include "context/editor/shell/welcome.h"
#include "context/editor/shell/window_bridge.h"
#include "context/editor/shell/window_registry.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace smoke = context::editor::shell::smoke;
namespace client = context::editor::client;
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
    std::fprintf(stderr, "[editor-cef-smoke-shell-inspector-fanout] FAIL (line %d): %s\n", line, what);
    ++g_failures;
}

#define SMOKE_CHECK(cond, what) check((cond), (what), __LINE__)

std::uint64_t now_us()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::int64_t now_ms()
{
    return static_cast<std::int64_t>(now_us() / 1000);
}

// See the sibling smokes: an empty fallback rather than an #error, so the pre-push audit's check 9
// can compile this TU standalone against the pinned CEF headers with none of CMake's defines. The
// real guards are cef/CMakeLists.txt's configure-time FATAL_ERROR and the runtime assertions below.
#if !defined(CONTEXT_WEBUI_ASSET_DIR)
#define CONTEXT_WEBUI_ASSET_DIR ""
#endif
// The `context` binary this smoke spawns a daemon from — threaded in by CMake as
// `$<TARGET_FILE:context>`, exactly as the `m*-exit-*` integration drills receive it. The empty
// fallback is for check 9 only; `main` asserts it is non-empty before using it.
#if !defined(CONTEXT_BINARY)
#define CONTEXT_BINARY ""
#endif

// Pinned for the same reason every sibling pins it: a CI host has no `prefers-color-scheme`, so
// leaving the choice ambient would make editor-core's own first-run rule (06 §4) an input to the
// test. Nothing here asserts pixels, but both windows boot the same deterministic path.
constexpr const char* kSmokeThemeId = "builtin.dark";

// The authored surface. The daemon roots its FileStore at the project dir and jails the reconcile
// crawl to `proj/` (daemon_command.cpp), so every authored path is `proj/<file>` — the SAME string
// is the compose resolver's key and the kernel's write path, which is what makes the write land where
// the read looked. Root INSTANCES child, so an `outermost` override lands in root.scene.json, a file
// the panel never names — the composed-write mode the Inspector actually uses.
constexpr const char* kRootScene = "proj/root.scene.json";
constexpr const char* kIdentity = "aaaaaaaaaaaaaaa1/ccccccccccccccc1";
constexpr const char* kFovPointer = "/components/camera/fov";
// Window A types this; window B must end up displaying it. A value whose canonical form differs from
// its authored one (`1`) so a stale render cannot pass for a fresh one.
constexpr const char* kTypedValue = "2.5";
// Window B stages this while window A writes, in §6. It must NEVER reach disk.
constexpr const char* kNeverWritten = "9.75";
// What window A writes in §6, racing window B's staged gesture. It must SURVIVE.
constexpr const char* kRaceValue = "6.5";

[[nodiscard]] bool write_file(const std::filesystem::path& path, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // C++ streams, never raw C stdio: MSVC `/W4 /WX` rejects `fopen` as C4996 (conventions.md).
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out << text;
    return out.good();
}

// Read an authored file back with a plain stream — REAL DISK, not a seam.
//
// ⚠ SCOPED DELIBERATELY, and every caller must keep it that way. filesync's atomic_write is
// temp + rename, and on Windows a rename over a target that still has an OPEN READ HANDLE fails,
// surfacing as a bare `internal.error` from the NEXT write which reads exactly like a product defect
// and is not one (conventions.md records the full triage). Returning by value closes the handle
// before the caller's next write.
[[nodiscard]] std::string read_authored(const std::filesystem::path& project, const char* name)
{
    std::ifstream in(project / "proj" / name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

[[nodiscard]] bool mentions(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] bool seed_project(const std::filesystem::path& project)
{
    const std::filesystem::path authored = project / "proj";
    const bool child = write_file(authored / "child.scene.json", R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "ccccccccccccccc1", "name": "Cam",
     "components": {
       "transform": {"position": [1, 2, 3]},
       "camera": {"fov": 1.0, "near": 0.1, "far": 500.0}
     }}
  ]})");
    const bool root = write_file(authored / "root.scene.json", R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [],
  "instances": [{"id": "aaaaaaaaaaaaaaa1", "scene": "proj/child.scene.json"}]})");
    return child && root;
}

// ---------------------------------------------------------------- one window's bridge surfaces
//
// Everything editor-core calls during boot, for ONE window, plus THIS window's own panel bag. The
// router denies unknown methods by default, so a missing surface here is a `refused()` — which this
// smoke asserts stays at zero for EVERY window. Each window gets its own instance: that is what "its
// own fresh editor-core instance" means on the native side (03 §1).
//
// MEMBER ORDER: `handshake` first so it is destroyed LAST (the router's handlers captured it, and the
// router lives outside this object, in the session the registry retires — window_registry.h §
// LIFETIME RULE); `panel_host` before `builtin`, which takes a reference to it.
struct WindowSurfaces
{
    shell::ShellHandshake handshake{shell::make_handshake_nonce()};
    shell::PanelHost panel_host;
    panels::BuiltinPanels builtin = panels::install_builtin_panels(panel_host);
    shell::EditorStateBridge editor_state;
    shell::KeybindingsBridge keybindings;
    shell::ThemesBridge themes;
    shell::WelcomeBridge welcome;
    shell::BannerBridge banners;
    shell::UserConfigStore config;
    // Installed UNBOUND, exactly as every sibling smoke installs it: the router denies unknown
    // methods by DEFAULT, so an uninstalled surface editor-core's boot calls is an `unknown_method`
    // REFUSAL that would trip this smoke's `refused() == 0` invariant. Unbound serves the honest
    // `edit` / `attached:false` baseline; the daemon-driven play state is e08d's own smoke's subject.
    shell::SessionBridge session_bridge;
    // e13c-4: editor-core's boot reads the operator's install-consent answers with
    // `package.grants.list` before any panel mounts (boot.ts, `ShellPackageGrants.load`), for the
    // same deny-by-default reason as every surface above — uninstalled it is an `unknown_method`
    // REFUSAL that trips this smoke's `refused() == 0` invariant on EVERY window. Bound to an EMPTY
    // scan (no package is installed here, so no contribution can carry a grant) and an EMPTY grants
    // path (deterministic regardless of the host's own `~/.context/package-grants.json`, and
    // `PackageGrantStore::save` REFUSES an empty path, so a `decide` can never write a developer's
    // real consent document), so the served answer is the same deny-all state a refusal produces and
    // NO package gains authority. `package_scan` is declared BEFORE the host that references it, so
    // it outlives it; the host is built in `install` for the same reason `window_bridge` is — it has
    // no default constructor.
    shell::PackageStoreScan package_scan;
    std::unique_ptr<shell::PackageGrantHost> package_grants;
    std::unique_ptr<shell::WindowBridge> window_bridge;

    // All the empty per-user paths are deliberate — a permanently-absent per-user file makes the boot
    // deterministic regardless of what the CI host has in `~/.context/` (the sibling smokes'
    // rationale, verbatim).
    [[nodiscard]] bool install(shell::BridgeRouter& router, shell::WindowManager& manager,
                               shell::WindowMoveStore& store, shell::WindowId window_id)
    {
        bool ok = handshake.install(router);
        ok = panel_host.install(router) && ok;
        editor_state.bind_store(&manager.state_store(), now_us);
        editor_state.bind_regions(
            [&manager, window_id](std::vector<shell::ShellRegion> regions)
            {
                if (shell::EditorWindow* target = manager.window(window_id))
                {
                    target->input().regions().publish(std::move(regions));
                }
            });
        ok = editor_state.install(router) && ok;
        keybindings.bind_path(std::filesystem::path{});
        ok = keybindings.install(router) && ok;
        themes.bind_directory(std::filesystem::path{});
        ok = themes.install(router) && ok;
        welcome.set_launch_mode(shell::LaunchMode::project);
        welcome.set_config_path(std::filesystem::path{});
        ok = welcome.install(router) && ok;
        ok = banners.install(router) && ok;
        config.bind_path(std::filesystem::path{});
        ok = config.install(router) && ok;
        ok = session_bridge.install(router) && ok;
        // d1: install() also routes `session.control` — session_bridge.h § kSessionControlMethod.
        ok = router.has_method(shell::kSessionControlMethod) && ok;
        // d3: install() also routes `session.select` (selection.clear's relay) — the ten-smoke
        // rule asserts the ROUTING here, per smoke (`menu.publish`'s check sits after the window
        // bridge installs below, which is what routes it).
        ok = router.has_method(shell::kSessionSelectMethod) && ok;
        package_grants =
            std::make_unique<shell::PackageGrantHost>(package_scan, std::filesystem::path{});
        ok = package_grants->install(router) && ok;
        window_bridge = std::make_unique<shell::WindowBridge>(window_id, store);
        ok = window_bridge->install(router) && ok;
        // d3: the window bridge's install() also routes `menu.publish` (the ten-smoke rule).
        ok = router.has_method(shell::kMenuPublishMethod) && ok;
        return ok;
    }
};

std::string boot_url()
{
    return std::string(shell::kAppEntryUrl) + "?" + shell::kThemePinFlag + "=" + kSmokeThemeId;
}

shell::cef::CefShellOptions make_cef_options(const smoke::BrowserGeometry& geometry,
                                             shell::BridgeRouter* bridge)
{
    shell::cef::CefShellOptions options;
    // Windowless in BOTH window modes: cef_shell.cpp reads `native_window` only under _WIN32, so
    // elsewhere CEF stays windowless-OSR either way and the Shell's own window is the PRESENT target.
    options.native_window = nullptr;
    options.logical_size = geometry.logical_size;
    options.dpi = geometry.dpi;
    options.url = boot_url();
    options.app_asset_root = CONTEXT_WEBUI_ASSET_DIR;
    options.bridge = bridge;
    options.windowless_frame_rate = 10;
    // Isolate the OSCrypt profile-encryption key from the MACHINE keychain (issue #437). Without
    // this, macOS blocks CefShutdown() forever on a SecurityAgent authorization prompt no automated
    // run can answer, so the smoke prints its whole verdict and then never exits — see
    // CefShellOptions::use_mock_keychain for the mechanism. EVERY CEF smoke sets it, and
    // tools/check_cef_keychain_isolation.py fails the build if one stops.
    options.use_mock_keychain = true;
    return options;
}

int finish(int code)
{
#if defined(_WIN32)
    // Session-0 carve-out (mirrors the sibling smokes): CEF's teardown is flaky on the self-hosted
    // Windows runner, so exit hard once the verdict is decided.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
#else
    return code;
#endif
}

// --------------------------------------------------------------- the DOM half, as real JS
//
// THE ONE PLACE THIS SMOKE TOUCHES THE RENDERER, and it touches it the way a human does. The
// hydration runtime rewrites each model node's `id` into `data-node-id` when it mounts
// (hydration.ts), so the field's element is addressed by the node id `InspectorPanel::build_panel`
// gave it: `<kInspectorWidgetNodePrefix><pointer>`. Both scripts are TOTAL — a missing element is a
// silent no-op, because they run on a schedule this file controls and the panel may not have been
// rendered yet, and the WAIT is what turns that into a legible failure instead of a thrown exception
// in a renderer with no console.
//
// ⚠ WHY TWO SCRIPTS AND NOT ONE. `input` STAGES (05 §8's "DOM input -> command inspector.edit") and
// `change` ENDS THE GESTURE (its "gesture end -> commit"). Dispatching both in one script would make
// a single C++ observation ("something was written") cover both links, so a regression that lost the
// staging channel and committed straight from `change` would still pass. Split, each link has its own
// observation: `has_staged_edit` after the first, `commits_observed` after the second.
// The `data-node-id` selector every script below addresses the field through. Built ONCE so the three
// scripts cannot disagree about it — and spelled with ordinary escaped literals rather than a raw
// string, which is not a style preference: the first version of this file assembled these scripts from
// `R"JS(...)JS"` chunks and left ONE stray `'` before a terminator, so the selector's string literal
// closed a character early. Every script then died in the renderer with `Uncaught SyntaxError: missing
// ) after argument list` while every C++ wait below timed out reporting that the DOM "never" did
// something — a broken TEST wearing the exact symptoms of a broken product. What identified it in one
// pass was the Shell's own `OnConsoleMessage` logging (test.md § Suite 1: "a live CEF smoke must report
// a CAUSE, not just a verdict"), and `drive_dom` now also dumps the script it injected, so the next
// reader does not have to reconstruct it from a diff.
[[nodiscard]] std::string widget_selector()
{
    return std::string("[data-node-id=\"") + panels::kInspectorWidgetNodePrefix + kFovPointer + "\"]";
}

[[nodiscard]] std::string stage_script(const std::string& value)
{
    return "(function () {\n"
           "    var node = document.querySelector('" +
           widget_selector() + "');\n"
           "    if (node === null) { return; }\n"
           "    node.value = \"" +
           value + "\";\n"
           "    node.dispatchEvent(new Event(\"input\", { bubbles: true }));\n"
           "})();";
}

// Dispatch `input` with the value UNTOUCHED — `commandValueFor` then reads what the element is
// DISPLAYING (`control.value`, which reflects the `value` attribute the patcher wrote while the user
// has typed nothing) and stages THAT. This is the DOM READ-BACK: the value that lands in the C++
// model came out of the rendered DOM, so asserting on it is asserting about the surface a human sees.
[[nodiscard]] std::string readback_script()
{
    return "(function () {\n"
           "    var node = document.querySelector('" +
           widget_selector() + "');\n"
           "    if (node === null) { return; }\n"
           "    node.dispatchEvent(new Event(\"input\", { bubbles: true }));\n"
           "})();";
}

// A DOM PROBE that reports through `console.log`, which the Shell already forwards to stderr
// (`OnConsoleMessage`). The renderer is otherwise a black box from here: the two `drive_dom` failure
// counters can say "no `panel.command` reached the host" but not WHY, and the three candidate causes
// — the element is absent (the panel is not mounted), it carries no `data-command`, or the runtime's
// own command set does not list `inspector.edit` — are indistinguishable from the C++ side. Injected
// on the failure path only, so a green run pays nothing for it.
[[nodiscard]] std::string probe_script()
{
    return "(function () {\n"
           "    var all = document.querySelectorAll('[data-node-id]');\n"
           "    var node = document.querySelector('" +
           widget_selector() + "');\n"
           "    var report = {\n"
           "        nodes: all.length,\n"
           "        found: node !== null,\n"
           "        command: node === null ? null : node.getAttribute('data-command'),\n"
           "        tag: node === null ? null : node.tagName,\n"
           "        value: node === null ? null : node.value,\n"
           "        defaultValue: node === null ? null : node.defaultValue,\n"
           "        ids: Array.prototype.slice.call(all).map(function (e) {\n"
           "            return e.getAttribute('data-node-id');\n"
           "        }).slice(0, 64)\n"
           "    };\n"
           "    console.log('[fanout-probe] ' + JSON.stringify(report));\n"
           "})();";
}

[[nodiscard]] std::string commit_script()
{
    return "(function () {\n"
           "    var node = document.querySelector('" +
           widget_selector() + "');\n"
           "    if (node === null) { return; }\n"
           "    node.dispatchEvent(new Event(\"change\", { bubbles: true }));\n"
           "})();";
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

    // Parsed AFTER the subprocess re-entry: a CEF child inherits the flag on its command line and
    // must never reach this body.
    const smoke::WindowMode window_mode = smoke::window_mode_from_args(argc, argv);

    std::printf("[editor-cef-smoke-shell-inspector-fanout] design 05 §8 end to end: a real DOM edit "
                "in window A reaches window B's Inspector through a real daemon\n");

    SMOKE_CHECK(std::string(CONTEXT_WEBUI_ASSET_DIR).empty() == false,
                "CONTEXT_WEBUI_ASSET_DIR was compiled in (the webui asset root is wired)");
    SMOKE_CHECK(std::string(CONTEXT_BINARY).empty() == false,
                "CONTEXT_BINARY was compiled in (the daemon this smoke spawns)");
    if (std::string(CONTEXT_WEBUI_ASSET_DIR).empty() || std::string(CONTEXT_BINARY).empty())
    {
        return finish(1);
    }

    std::error_code ec;
    const std::filesystem::path project =
        std::filesystem::temp_directory_path(ec) / "context-editor-cef-inspector-fanout-smoke";
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);
    SMOKE_CHECK(seed_project(project), "the two-file composed project was seeded on real disk");

    const render::Extent2D size{640, 480};

    // --- the daemon link: the PRODUCTION spine, not a hand-rolled child ---------------------------
    //
    // `DaemonLifecycle::spawn_or_attach` resolves the project, spawns `context daemon` as a child and
    // reads the D20 attach token off its STDOUT (never argv/env — 05 §2 / 08 threat model). Using the
    // real spine rather than a bespoke spawn is what makes this smoke's daemon the one the shipping
    // editor gets, including its subscription, its reconnect ladder and its exit policy.
    //
    // DECLARED BEFORE the manager and the routers so it OUTLIVES neither: it is torn down explicitly
    // at the end, after CEF is gone, and the bags' non-owning client views are cleared first.
    shell::DaemonLifecycle lifecycle;
    // The SAME topic set editor_main.cpp subscribes (`kSessionTopic` included, so the two windows'
    // Session feeds see daemon selection truth): a smoke on a narrower set would prove the fan-out
    // over a stream the product does not use.
    lifecycle.set_subscription_topics({panels::kDiagnosticsTopic, panels::kDerivationTopic,
                                      panels::kSessionTopic});

    // EVERY window's bag, in creation order. The event handler below iterates it, which is exactly
    // what editor_main.cpp now does: design 05 §8's tail says "all subscribed clients (window 1,
    // window 2, …) update", and a handler wired to ONE bag makes that false for every other window.
    // Raw pointers are safe here because a window's session is RETIRED, never freed, and this smoke
    // destroys no window at all.
    std::vector<WindowSurfaces*> fed;

    // ⚠ WIRING PARITY ONLY — this handler is NOT exercised by this scenario, and saying so is cheaper
    // than letting a future reader infer coverage from its presence. `fed` is still EMPTY when
    // `spawn_or_attach` below delivers the initial subscription snapshot (window A is appended after
    // it, window B later still), and nothing here forces a reconnect, so the body never runs. It
    // mirrors `editor_main.cpp`, where window 0's bag IS a captured pointer that is live at attach
    // time and so does receive it. Reordering the smoke to cover this would mean building a window
    // before the daemon exists; the snapshot path's coverage is the Problems feed's own tier.
    lifecycle.on_snapshot(
        [&fed](const std::string&, const context::editor::contract::Json& snapshot)
        {
            for (WindowSurfaces* surfaces : fed)
            {
                if (surfaces->builtin.problems != nullptr)
                {
                    panels::apply_problems_snapshot(*surfaces->builtin.problems, snapshot, 0);
                }
            }
        });
    lifecycle.on_event(
        [&fed](const std::string&, const client::ClientEvent& event)
        {
            for (WindowSurfaces* surfaces : fed)
            {
                panels::BuiltinPanels& bag = surfaces->builtin;
                if (bag.problems != nullptr)
                {
                    (void)panels::apply_problems_event(*bag.problems, event.topic, event.payload,
                                                       event.generation);
                }
                if (bag.scenetree != nullptr)
                {
                    (void)panels::apply_scenetree_event(*bag.scenetree, event.topic, event.payload,
                                                        event.generation);
                }
                // The fan-out under test. A `false` return is ORDINARY — the feed WITHHOLDS the
                // re-read while a gesture is staged (inspector_feed.h § apply_event), which §6 below
                // asserts on directly.
                if (bag.inspector != nullptr)
                {
                    (void)panels::apply_inspector_event(*bag.inspector, event.topic, event.payload);
                }
                if (bag.session != nullptr)
                {
                    (void)panels::apply_session_event(*bag.session, event.topic, event.payload);
                }
            }
        });

    std::string daemon_error;
    const bool attached =
        lifecycle.spawn_or_attach(project, std::filesystem::path(CONTEXT_BINARY), daemon_error);
    SMOKE_CHECK(attached, "a real daemon was spawned and attached (the D20 token came over stdio)");
    if (!attached)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-inspector-fanout] daemon: %s\n",
                     daemon_error.c_str());
        std::filesystem::remove_all(project, ec);
        return finish(1);
    }
    SMOKE_CHECK(lifecycle.owns_daemon(), "this process OWNS the daemon it spawned");
    SMOKE_CHECK(lifecycle.token_via_stdio(), "the attach token was read off the child's stdout");
    SMOKE_CHECK(lifecycle.client() != nullptr && lifecycle.client()->client_id() > 0,
                "the client has a daemon-minted id");

    // --- window A (the primary), built exactly as editor_main.cpp does ----------------------------
    //
    // The move store is declared FIRST so it outlives every window's WindowBridge, including those in
    // retired sessions the manager frees only in ~WindowManager. The primary's router + surfaces are
    // declared before the manager so its bridge outlives every handler AND the manager's own
    // teardown — `shell::cef::shutdown()` below runs while both are still in scope, which is the
    // CE #319 invariant `cef_shell.h` states.
    // The window factory's two out-params, declared HERE — ahead of `manager` — deliberately. The
    // factory lambda captures them by REFERENCE and is STORED INSIDE `manager`, so a declaration after
    // `manager` would let the referents die first at scope exit. Benign today (nothing invokes the
    // factory after window B is created, and ~WindowManager creates no window), but this file is
    // meticulous about exactly this class of ordering everywhere else, and a latent one is still worth
    // not having.
    WindowSurfaces* created_surfaces = nullptr;
    shell::BridgeRouter* created_bridge = nullptr;
    shell::WindowMoveStore move_store;
    shell::BridgeRouter primary_bridge;
    WindowSurfaces primary_surfaces;

    shell::WindowManager manager(project);

    SMOKE_CHECK(primary_surfaces.install(primary_bridge, manager, move_store, shell::kPrimaryWindowId),
                "every bridge surface installed on window A");
    fed.push_back(&primary_surfaces);

    {
        shell::WindowDesc desc;
        desc.title = "Context Editor (inspector fan-out smoke) — window A";
        desc.logical_size = size;
        smoke::WindowSetup window_setup = smoke::make_smoke_window(desc, window_mode);
        if (window_setup.backend == nullptr)
        {
            std::fprintf(stderr,
                         "[editor-cef-smoke-shell-inspector-fanout] FAIL: no %s window A: %s\n",
                         smoke::to_string(window_mode), window_setup.diagnostic.c_str());
            return finish(1);
        }
        const smoke::BrowserGeometry geometry = smoke::browser_geometry(*window_setup.backend);

        std::string error;
        std::unique_ptr<shell::IBrowserHost> browser =
            shell::cef::make_cef_browser_host(make_cef_options(geometry, &primary_bridge), error);
        if (browser == nullptr)
        {
            std::fprintf(stderr,
                         "[editor-cef-smoke-shell-inspector-fanout] FAIL: window A's browser did "
                         "not start: %s\n",
                         error.c_str());
            return finish(1);
        }

        shell::EditorWindowConfig config;
        config.compositor.import_options.force_software = true;
        config.placement_poll_us = 0;
        auto window = std::make_unique<shell::EditorWindow>(std::move(window_setup.backend),
                                                            std::move(browser), config);
        const smoke::PresentSetup present_setup = smoke::attach_smoke_present(*window, window_mode);
        if (!present_setup.ok)
        {
            std::fprintf(stderr,
                         "[editor-cef-smoke-shell-inspector-fanout] FAIL: no %s present path for "
                         "window A: %s\n",
                         smoke::to_string(window_mode), present_setup.diagnostic.c_str());
            return finish(1);
        }
        manager.add(std::move(window));
    }

    shell::EditorWindow* window_a = manager.window(shell::kPrimaryWindowId);
    SMOKE_CHECK(window_a != nullptr, "the manager adopted window A as the primary");
    if (window_a == nullptr)
    {
        return finish(1);
    }

    // --- the window factory: how window B is built (the same shape editor_main.cpp binds) ---------
    // Its two out-params are declared above `manager`, on purpose — see there.
    manager.bind_window_factory(
        [&](const shell::WindowSpec& spec, shell::WindowSessionParts& parts,
            std::string& error) -> bool
        {
            shell::WindowDesc desc;
            desc.title = spec.title;
            desc.logical_size = spec.logical_size;
            const smoke::WindowMode child_mode =
                spec.headless ? smoke::WindowMode::headless : window_mode;
            smoke::WindowSetup child_setup = smoke::make_smoke_window(desc, child_mode);
            if (child_setup.backend == nullptr)
            {
                error = child_setup.diagnostic;
                return false;
            }
            parts.backend = std::move(child_setup.backend);

            auto window_bridge = std::make_unique<shell::BridgeRouter>();
            auto surfaces = std::make_shared<WindowSurfaces>();
            const shell::WindowId expected_id =
                static_cast<shell::WindowId>(manager.last_minted_id() + 1u);
            if (!surfaces->install(*window_bridge, manager, move_store, expected_id))
            {
                error = "a bridge surface refused to install on the new window";
                return false;
            }

            std::string browser_error;
            parts.browser = shell::cef::make_cef_browser_host(
                make_cef_options(smoke::browser_geometry(*parts.backend), window_bridge.get()),
                browser_error);
            if (parts.browser == nullptr)
            {
                error = "the browser did not start: " + browser_error;
                return false;
            }

            created_surfaces = surfaces.get();
            created_bridge = window_bridge.get();
            parts.surfaces.push_back(std::move(surfaces));
            parts.bridge = std::move(window_bridge);
            error.clear();
            return true;
        });

    // ONE frame of the owner loop, in editor_main.cpp's ORDER: drive the daemon link (which
    // dispatches events into every bag), re-derive each bag's non-owning client views immediately
    // after the one call that can destroy the client, then drain each bag's due hydration work.
    // Returns false when no window is left, which every wait below tracks.
    const auto pump_once = [&]() -> bool
    {
        if (!manager.pump_once(now_us()))
        {
            return false;
        }
        lifecycle.pump(now_ms());
        client::Client* live = lifecycle.client();
        for (WindowSurfaces* surfaces : fed)
        {
            panels::bind_write_client(surfaces->builtin, live);
            if (surfaces->builtin.session != nullptr)
            {
                panels::bind_session_client(*surfaces->builtin.session, live);
            }
        }
        if (live != nullptr)
        {
            for (WindowSurfaces* surfaces : fed)
            {
                panels::pump_panel_feeds(surfaces->builtin, *live, kRootScene);
            }
        }
        return true;
    };

    // Drive until a window's editor-core has booted: a composited OSR frame AND a completed
    // handshake. Waiting on BOTH is what makes the wait non-vacuous — the frame proves the scheme
    // served a renderable document, the handshake proves the bundle executed and a value made the
    // full native->JS->native round trip.
    const auto boot_window = [&](shell::WindowId id, WindowSurfaces& surfaces, int seconds) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!pump_once())
            {
                return false;
            }
            shell::EditorWindow* window = manager.window(id);
            if (window == nullptr)
            {
                return false;
            }
            if (window->compositor().stats().view_frames > 0 && surfaces.handshake.complete())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    // Pump until `ready` holds, bounded so a regression is a legible failure instead of a ctest
    // Timeout — which, per test.md § Suite 1, cannot be told apart from a teardown wedge.
    const auto pump_until = [&](const char* what, int seconds, auto&& ready) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!pump_once())
            {
                std::fprintf(stderr,
                             "[editor-cef-smoke-shell-inspector-fanout] pump died waiting for %s\n",
                             what);
                return false;
            }
            if (ready())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::fprintf(stderr, "[editor-cef-smoke-shell-inspector-fanout] timed out waiting for %s\n",
                     what);
        return false;
    };

    // QUIESCE one window's Inspector before a gesture is staged in it: pump until no fetch is armed.
    //
    // ⚠ THIS IS NOT DECORATION, and it took a run to find. `InspectorFeed::request`'s L-30 deferral
    // guards a re-read ARMED WHILE a gesture is staged — but a fetch armed BEFORE the gesture (this
    // window's own read-your-writes re-read, or the self-echo settle x9 now publishes) is already in
    // `pending_`, and `pump_panel_feeds` serves it on the next frame regardless: `apply_result` ->
    // `set_model` -> the staged edit is DISCARDED. The subsequent `commit` then finds nothing staged,
    // answers `Status::none`, fires no commit listener, and every wait keyed on `commits_observed`
    // times out with a message about the DOM never committing. That is exactly the shape MEASURED on
    // this file's second run, and it made §3 look intermittent.
    //
    // In the shipping editor the window is ~one 4 ms frame wide, so a human cannot realistically stage
    // inside it — which is why this is a determinism fix for the TEST rather than a product change.
    // Recorded here because the reasoning is not obvious from either side of the seam.
    const auto quiesce = [&](const char* which, panels::BuiltinPanels& bag, int seconds) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!pump_once())
            {
                return false;
            }
            if (!panels::observe_inspector(bag).fetch_pending)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-inspector-fanout] %s never quiesced (a fetch stayed "
                     "armed)\n",
                     which);
        return false;
    };

    // Re-run `script` in `window` until `ready()` holds. The renderer may not have MOUNTED the
    // Inspector's field yet when the first attempt lands (its model is hydrated by C++ above and
    // reaches the DOM only on editor-core's next `panel.render` poll), and both scripts are total
    // no-ops against a missing element — so retrying is how "the element was not there yet" stops
    // being indistinguishable from "the event did nothing".
    const auto drive_dom = [&](const char* what, shell::EditorWindow& window,
                               WindowSurfaces& surfaces, const shell::BridgeRouter& router,
                               const std::string& script, int seconds, auto&& ready) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            window.browser().execute_script(script);
            for (int i = 0; i < 20; ++i)
            {
                if (!pump_once())
                {
                    std::fprintf(stderr,
                                 "[editor-cef-smoke-shell-inspector-fanout] pump died driving %s\n",
                                 what);
                    return false;
                }
                if (ready())
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        // The script VERBATIM plus the four counters that tell a reader WHERE the dispatch stopped,
        // because "the DOM never did X" is the one message equally consistent with a product
        // regression and with a typo in the JS above — and the second is what actually happened the
        // first time this file ran (see widget_selector's note). What each one discriminates:
        //   renders_served == 0        -> editor-core never even fetched a render; the panel is not
        //                                 mounted, so the element the script looks for cannot exist.
        //   commands_dispatched == 0   -> the render arrived but no `panel.command` reached the host:
        //                                 the DOM event did not turn into a dispatch (a selector that
        //                                 matches nothing, a `data-command` that is absent, a
        //                                 `#commands` set that does not carry `inspector.edit`).
        //   refused > 0                -> a call DID reach the router and the router said no; the
        //                                 method name or the params are wrong, not the DOM.
        //   commands_dispatched > 0 but nothing staged/committed -> the command reached the provider
        //                                 and the PROVIDER declined (no parseable value, or a gesture
        //                                 verb the manifest does not advertise).
        // Ask the renderer what it can actually see, and pump so its `console.log` reaches stderr.
        window.browser().execute_script(probe_script());
        for (int i = 0; i < 40; ++i)
        {
            if (!pump_once())
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const panels::InspectorObservation seen = panels::observe_inspector(surfaces.builtin);
        bool overridden = false;
        const std::string shown =
            panels::inspector_field_value(surfaces.builtin, kFovPointer, overridden);
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-inspector-fanout] the DOM never %s "
                     "(renders_served=%zu commands_dispatched=%zu lists_served=%zu refused=%zu "
                     "staged=%d staged_value='%s' model_value='%s' commits=%zu pending=%d "
                     "deferred=%d); the injected script was:\n%s\n",
                     what, surfaces.panel_host.renders_served(),
                     surfaces.panel_host.commands_dispatched(),
                     surfaces.panel_host.lists_served(), router.refused(),
                     static_cast<int>(seen.has_staged_edit), seen.staged_value.c_str(),
                     shown.c_str(), seen.commits_observed, static_cast<int>(seen.fetch_pending),
                     static_cast<int>(seen.refresh_deferred), script.c_str());
        return false;
    };

    SMOKE_CHECK(boot_window(shell::kPrimaryWindowId, primary_surfaces, 60),
                "window A composited a live CEF frame and completed its bridge handshake");
    if (manager.window(shell::kPrimaryWindowId) != window_a)
    {
        std::fprintf(stderr, "[editor-cef-smoke-shell-inspector-fanout] FAIL: window A did not "
                             "survive its own boot\n");
        return finish(1);
    }

    // --- window B: a SECOND real browser booting its OWN editor-core ------------------------------
    shell::WindowSpec spec;
    spec.title = "Context Editor (inspector fan-out smoke) — window B";
    spec.logical_size = size;
    spec.headless = window_mode == smoke::WindowMode::headless;
    spec.state_index = 1;
    const shell::WindowCreateResult second = manager.create_window(spec, shell::kPrimaryWindowId);
    SMOKE_CHECK(second.ok(), "the registry created window B");
    // The factory RAN and handed back both out-params. Deliberately NOT phrased as
    // `created_bridge != &primary_bridge`, which was the original wording and CANNOT FAIL: a live heap
    // address can never equal the address of the live stack local `primary_bridge`, so that conjunct
    // was structurally incapable of reporting anything, and neither conjunct established the "its OWN
    // panel bag" half it claimed. What actually proves window B's router and bag are its own is that
    // they SERVED window B's own traffic — asserted on the counters after §5, once there is traffic to
    // count.
    SMOKE_CHECK(created_surfaces != nullptr && created_bridge != nullptr,
                "the factory produced window B's own bridge router and panel bag");
    if (!second.ok() || created_surfaces == nullptr)
    {
        return finish(1);
    }
    WindowSurfaces& b_surfaces = *created_surfaces;
    fed.push_back(&b_surfaces);
    if (shell::EditorWindow* window_b = manager.window(second.id))
    {
        const smoke::PresentSetup child_present =
            smoke::attach_smoke_present(*window_b, smoke::mode_of(window_b->backend()));
        SMOKE_CHECK(child_present.ok, "window B took its present path");
    }
    SMOKE_CHECK(boot_window(second.id, b_surfaces, 60),
                "window B composited a live CEF frame and completed its OWN bridge handshake");
    // A FRESH editor-core instance, not window A's document seen twice: the two Shells minted
    // DIFFERENT handshake nonces and only the document running in that window can round-trip its own.
    SMOKE_CHECK(b_surfaces.handshake.nonce() != primary_surfaces.handshake.nonce(),
                "the two windows minted different handshake nonces");
    SMOKE_CHECK(b_surfaces.handshake.nonce_mismatches() == 0 &&
                    primary_surfaces.handshake.nonce_mismatches() == 0,
                "neither window saw a replayed or guessed nonce");
    shell::EditorWindow* window_b = manager.window(second.id);
    if (window_b == nullptr)
    {
        return finish(1);
    }

    // === 1. BOTH windows' Inspectors are live on the SAME entity ==================================
    //
    // Armed through the ONE production seam (`InspectorFeed::request`, whose production driver is the
    // Scene tree's selection listener), then served by the per-window pump inside `pump_once`. Armed
    // ONCE, up front, and never moved again: selection is DAEMON state since e08b and moving it while
    // a gesture is staged trips CE #452 (see the header).
    SMOKE_CHECK(panels::request_inspector(primary_surfaces.builtin, kIdentity),
                "window A's Inspector armed a fetch for the entity");
    SMOKE_CHECK(panels::request_inspector(b_surfaces.builtin, kIdentity),
                "window B's Inspector armed a fetch for the entity");
    SMOKE_CHECK(pump_until("both Inspectors to hydrate", 30,
                           [&]
                           {
                               return panels::observe_inspector(primary_surfaces.builtin)
                                              .results_applied > 0 &&
                                      panels::observe_inspector(b_surfaces.builtin).results_applied >
                                          0;
                           }),
                "both windows' Inspectors hydrated over the live `editor.inspect` read");
    {
        const panels::InspectorObservation a = panels::observe_inspector(primary_surfaces.builtin);
        const panels::InspectorObservation b = panels::observe_inspector(b_surfaces.builtin);
        SMOKE_CHECK(a.present && b.present, "both bags actually have an Inspector feed");
        SMOKE_CHECK(a.has_selection && b.has_selection, "both Inspectors resolved the entity");
        // A real CAS token, from a real read of a real file — what makes the L-30 guard in §6 mean
        // something.
        SMOKE_CHECK(a.base_raw_hash != 0 && b.base_raw_hash != 0,
                    "both Inspectors adopted the root scene's raw-byte CAS token");
        bool overridden_a = false;
        bool overridden_b = false;
        SMOKE_CHECK(panels::inspector_field_value(primary_surfaces.builtin, kFovPointer,
                                                  overridden_a) == "1",
                    "window A shows the AUTHORED value");
        SMOKE_CHECK(panels::inspector_field_value(b_surfaces.builtin, kFovPointer, overridden_b) ==
                        "1",
                    "window B shows the AUTHORED value");
        SMOKE_CHECK(!overridden_a && !overridden_b, "neither shows it as an override yet");
    }

    // === 2. THE FIRST LINK: a REAL DOM `input` event STAGES on window A's model ===================
    //
    // Nothing in this file hands `kTypedValue` to any C++ function. It exists only inside the JS
    // string, so a staged edit carrying it can ONLY have travelled
    // DOM -> hydration `#stageFrom` -> `commandValueFor` -> `PanelClient.command(..., value)` ->
    // the CEF router -> `panel.command` -> the Inspector's provider -> `stage_edit`.
    SMOKE_CHECK(quiesce("window A", primary_surfaces.builtin, 30),
                "window A's Inspector was idle before the gesture (no fetch armed — see `quiesce`)");
    const std::size_t a_commits_before = panels::observe_inspector(primary_surfaces.builtin).commits_observed;
    SMOKE_CHECK(drive_dom("staged window A's edit", *window_a, primary_surfaces, primary_bridge,
                          stage_script(kTypedValue), 60,
                          [&]
                          { return panels::observe_inspector(primary_surfaces.builtin).has_staged_edit; }),
                "a REAL `input` event on the rendered field STAGED an edit on window A's model");
    {
        const panels::InspectorObservation a = panels::observe_inspector(primary_surfaces.builtin);
        SMOKE_CHECK(a.staged_pointer == kFovPointer,
                    "the staged edit names the field the element was rendered for");
        // THE assertion the whole DOM half turns on: the VALUE crossed the bridge. e09e-1's
        // `PanelClient.command` value parameter is the only thing that can carry it, and before
        // e09e-1 there was no parameter at all — so a regression that dropped it stages nothing (the
        // provider DECLINES a dispatch with no parseable value) and the wait above times out.
        SMOKE_CHECK(a.staged_value == kTypedValue,
                    "the staged value is the one TYPED INTO THE DOM, not one this file passed in");
        // Staging is not a write. If this were non-zero the `change` event below would be proving
        // nothing about the commit link.
        SMOKE_CHECK(a.writes_applied == 0, "staging wrote NOTHING to the daemon yet");
        SMOKE_CHECK(a.commits_observed == a_commits_before, "and resolved no commit");
    }

    // === 3. THE SECOND LINK: a REAL DOM `change` event COMMITS it over the wire ====================
    SMOKE_CHECK(drive_dom("committed window A's gesture", *window_a, primary_surfaces, primary_bridge,
                          commit_script(), 60,
                          [&]
                          {
                              return panels::observe_inspector(primary_surfaces.builtin)
                                         .commits_observed > a_commits_before;
                          }),
                "a REAL `change` event ENDED the gesture and drove the commit (L-20)");
    {
        const panels::InspectorObservation a = panels::observe_inspector(primary_surfaces.builtin);
        SMOKE_CHECK(a.last_commit_status == "applied",
                    "the commit APPLIED over the daemon's `edit` RPC");
        SMOKE_CHECK(a.writes_applied == 1, "exactly one override write reached the daemon");
        SMOKE_CHECK(!a.has_staged_edit, "the applied commit consumed the gesture");
    }
    // …and the bytes are on REAL DISK, in the file COMPOSITION chose — a path the panel never named
    // and the Shell cannot compute for itself (`context_compose` is D10-forbidden to it).
    {
        const std::string on_disk = read_authored(project, "root.scene.json");
        SMOKE_CHECK(mentions(on_disk, "\"overrides\""), "an override block was authored");
        SMOKE_CHECK(mentions(on_disk, kFovPointer), "…for the field the DOM edited");
        SMOKE_CHECK(mentions(on_disk, kTypedValue), "…carrying the value typed into the DOM");
    }

    // === 4. THE TAIL: the daemon's own settle reaches WINDOW B's Inspector ========================
    //
    // ⚠ THE ONLY TRIGGER IS WINDOW A'S PLAIN `edit` (M9 x9). No `reconcile` chaser, no `edit-batch`,
    // no `await_*`. So this is §8's chain reached by the exact call the editor makes — and if x9's
    // publisher regresses, `events_applied` never climbs and this section reds rather than going on
    // to prove that `reconcile` still works.
    SMOKE_CHECK(pump_until("window B's Inspector to fold in the settle", 30,
                           [&] { return panels::observe_inspector(b_surfaces.builtin).results_applied > 1; }),
                "window B re-read the entity off the daemon's `derivation.settled`");
    {
        const panels::InspectorObservation b = panels::observe_inspector(b_surfaces.builtin);
        // RECOGNIZED, and exactly ONCE for the one logical edit. The count is the storm check: an
        // edit that published two settles would double every window's re-read traffic.
        SMOKE_CHECK(b.events_applied == 1,
                    "window B recognized exactly ONE settle for window A's one edit");
        SMOKE_CHECK(b.rereads_armed > 0, "…and it armed the re-read");
        SMOKE_CHECK(!b.refresh_deferred, "nothing was withheld (no gesture was staged in B)");
        bool overridden = false;
        // THE FAN-OUT, in window B's MODEL: a value it never wrote and never re-selected for.
        SMOKE_CHECK(panels::inspector_field_value(b_surfaces.builtin, kFovPointer, overridden) ==
                        kTypedValue,
                    "window B's Inspector shows the value window A typed");
        SMOKE_CHECK(overridden, "…and shows it as an OVERRIDE, as the composed read reports it");
        // Window B wrote nothing at any point. Without this, a regression that somehow routed A's
        // write through B's gateway would still satisfy every value assertion above.
        SMOKE_CHECK(b.writes_applied == 0, "window B has written nothing of its own");
    }

    // === 5. THE DOM READ-BACK: window B's RENDERED element carries it too ========================
    //
    // The model is not the surface. Dispatching `input` on window B's field WITHOUT touching its
    // value makes `commandValueFor` read what the element is DISPLAYING and stage that, so the value
    // that arrives in B's C++ model came OUT of B's live DOM. That is what makes "window B's
    // Inspector reflects it" a claim about the thing a human looks at.
    SMOKE_CHECK(quiesce("window B", b_surfaces.builtin, 30),
                "window B's Inspector was idle before the read-back");
    // THE PREDICATE IS THE VALUE, not merely "something was staged", and that is deliberate: window
    // B's DOM catches up on its own schedule — the `pollRevisions` tick this task wired, or this very
    // dispatch's `onDispatched` refresh — so the first read-back attempt can legitimately land on a
    // DOM that still shows the pre-fan-out value. Keying on `has_staged_edit` alone accepted that
    // stale read and turned a race into a value mismatch reported one assertion later (MEASURED, this
    // file's third run: staged `1`, the authored value). Keying on the value makes `drive_dom` retry
    // until the DOM has caught up — and its deadline is what keeps a genuine failure to propagate a
    // RED rather than a hang.
    SMOKE_CHECK(drive_dom("read back window B's rendered value", *window_b, b_surfaces, *created_bridge,
                          readback_script(), 60,
                          [&]
                          {
                              const panels::InspectorObservation b =
                                  panels::observe_inspector(b_surfaces.builtin);
                              return b.has_staged_edit && b.staged_value == kTypedValue;
                          }),
                "window B's rendered field staged its own displayed value");
    {
        const panels::InspectorObservation b = panels::observe_inspector(b_surfaces.builtin);
        SMOKE_CHECK(b.staged_value == kTypedValue,
                    "window B's LIVE DOM is displaying the value window A typed");
        SMOKE_CHECK(b.staged_pointer == kFovPointer, "…on the field window A edited");
        // …AND IT WAS WINDOW B'S OWN HOST THAT DID THE WORK — the assertion the can't-fail pointer
        // comparison at window B's creation was reaching for. Both counters belong to window B's own
        // `PanelHost` and start at 0; `drive_dom`'s failure diagnostic prints exactly these two BECAUSE
        // a 0 discriminates "the panel never rendered" and "no `panel.command` ever reached this host",
        // so a wiring bug that pointed window B's browser at window A's router leaves them at 0 here.
        SMOKE_CHECK(b_surfaces.panel_host.renders_served() > 0,
                    "window B's OWN PanelHost served the renders its DOM was built from");
        SMOKE_CHECK(b_surfaces.panel_host.commands_dispatched() > 0,
                    "…and dispatched window B's own `inspector.edit`, on its own router");
    }
    // Discard it: a read-back must leave no gesture behind, and `cancel` is the verb that fires no
    // commit listener (which is exactly why `flush_deferred` has a call site there).
    {
        bool dispatched = false;
        std::string code;
        SMOKE_CHECK(b_surfaces.panel_host.gesture(panels::kInspectorPanelId,
                                                  shell::GestureVerb::cancel,
                                                  context::editor::contract::Json::object(),
                                                  dispatched, code),
                    "window B's read-back gesture was cancellable");
        SMOKE_CHECK(dispatched, "…and something was actually staged to cancel");
        SMOKE_CHECK(!panels::observe_inspector(b_surfaces.builtin).has_staged_edit,
                    "the read-back left no gesture behind");
    }

    // === 6. THE GUARD: a settle arriving MID-GESTURE must not re-base window B's L-30 base ========
    //
    // The keystone's dangerous direction. `InspectorPanel::set_model` — which a served re-read's own
    // `apply_result` calls — DISCARDS the staged edit AND adopts the fresh file as the new
    // `base_raw_hash`. Serving one mid-gesture would silently RE-BASE the L-30 collision guard: the
    // following commit would CAS against the concurrent writer's own post-write state, find no
    // mismatch, and OVERWRITE it. Nothing errors. Nothing crashes. A defeated compare-and-swap looks
    // exactly like a successful edit — so the assertions that matter here are the NEGATIVE ones, and
    // the on-disk pair at the end is what makes them detectors rather than decoration.
    const std::uint64_t b_base = panels::observe_inspector(b_surfaces.builtin).base_raw_hash;
    SMOKE_CHECK(b_base != 0, "window B holds a real CAS base going into the race");
    SMOKE_CHECK(quiesce("window B", b_surfaces.builtin, 30),
                "window B's Inspector was idle before the mid-gesture edit");
    SMOKE_CHECK(drive_dom("staged window B's mid-gesture edit", *window_b, b_surfaces, *created_bridge,
                          stage_script(kNeverWritten), 60,
                          [&] { return panels::observe_inspector(b_surfaces.builtin).has_staged_edit; }),
                "the human is mid-edit in window B");
    {
        const panels::InspectorObservation b = panels::observe_inspector(b_surfaces.builtin);
        SMOKE_CHECK(b.staged_value == kNeverWritten, "window B's staged value is the one it typed");
        SMOKE_CHECK(b.base_raw_hash == b_base, "staging did not move the CAS base");
    }
    {
        const panels::InspectorObservation before = panels::observe_inspector(b_surfaces.builtin);
        const std::size_t a_commits = panels::observe_inspector(primary_surfaces.builtin).commits_observed;

        // Window A moves THE SAME FIELD while window B's gesture is in flight — through its own real
        // DOM again, so the racing write is as real as the raced one.
        SMOKE_CHECK(quiesce("window A", primary_surfaces.builtin, 30),
                    "window A's Inspector was idle before the racing edit");
        SMOKE_CHECK(drive_dom("staged window A's racing edit", *window_a, primary_surfaces,
                              primary_bridge, stage_script(kRaceValue), 60,
                              [&]
                              {
                                  const panels::InspectorObservation a =
                                      panels::observe_inspector(primary_surfaces.builtin);
                                  return a.has_staged_edit && a.staged_value == kRaceValue;
                              }),
                    "window A staged the racing edit");
        SMOKE_CHECK(drive_dom("committed window A's racing edit", *window_a, primary_surfaces,
                              primary_bridge, commit_script(), 60,
                              [&]
                              {
                                  return panels::observe_inspector(primary_surfaces.builtin)
                                             .commits_observed > a_commits;
                              }),
                    "window A committed the racing edit");
        SMOKE_CHECK(panels::observe_inspector(primary_surfaces.builtin).last_commit_status ==
                        "applied",
                    "window A's racing edit APPLIED (nobody had moved the file under it)");

        // The settle for THAT write must reach window B and be RECOGNIZED — otherwise every negative
        // below passes for the one reason that would make this whole section vacuous: the fact never
        // arrived at all.
        SMOKE_CHECK(pump_until("window B to recognize the mid-gesture settle", 30,
                               [&]
                               {
                                   return panels::observe_inspector(b_surfaces.builtin)
                                              .events_applied > before.events_applied;
                               }),
                    "window B RECOGNIZED the settle that arrived mid-gesture");

        const panels::InspectorObservation b = panels::observe_inspector(b_surfaces.builtin);
        SMOKE_CHECK(b.refresh_deferred, "…and knowingly WITHHELD the re-read it owes");
        SMOKE_CHECK(!b.fetch_pending, "no fetch was armed");
        SMOKE_CHECK(b.rereads_armed == before.rereads_armed, "no re-read was armed");
        SMOKE_CHECK(b.has_staged_edit, "the human's in-flight edit SURVIVED");
        SMOKE_CHECK(b.staged_value == kNeverWritten, "…unchanged");
        SMOKE_CHECK(b.base_raw_hash == b_base, "…and its L-30 CAS base was NOT re-based");
        SMOKE_CHECK(b.results_applied == before.results_applied,
                    "window B applied no fresh model across the pumps that would have done the damage");
    }

    // THE PAYOFF: the L-30 guarantee is intact, so window B's commit CAS-fails, re-reads, sees the
    // field MOVED, and DROPS — window A's value survives and window B's is never written.
    {
        bool dispatched = false;
        std::string code;
        SMOKE_CHECK(b_surfaces.panel_host.gesture(panels::kInspectorPanelId,
                                                  shell::GestureVerb::commit,
                                                  context::editor::contract::Json::object(),
                                                  dispatched, code),
                    "window B's mid-gesture commit was dispatchable");
        SMOKE_CHECK(dispatched, "…and found a staged gesture to commit");
        const panels::InspectorObservation b = panels::observe_inspector(b_surfaces.builtin);
        SMOKE_CHECK(b.last_commit_status == "dropped", "window B's commit was LOUDLY DROPPED (L-30)");
        // Deliberately NOT redundant with the line above, because SMOKE_CHECK is non-fatal: `none` is
        // the specific status a SERVED settle produces (nothing staged left to commit), so naming it
        // makes a failure say WHICH way it broke. Do not simplify this away.
        SMOKE_CHECK(b.last_commit_status != "none",
                    "…not `none`, which is what a served mid-gesture refresh would have produced");
        SMOKE_CHECK(b.last_commit_code == "cas.mismatch", "…with the L-30 collision code");
        SMOKE_CHECK(b.drops_observed == 1, "…counted as exactly one drop");
        SMOKE_CHECK(b.writes_applied == 0, "and window B still wrote NOTHING");
    }
    {
        const std::string on_disk = read_authored(project, "root.scene.json");
        SMOKE_CHECK(mentions(on_disk, kRaceValue), "window A's racing value is on disk");
        SMOKE_CHECK(!mentions(on_disk, kNeverWritten),
                    "window B's defeated gesture never reached disk");
    }

    // === 7. …AND WINDOW B DOES REFRESH ONCE ITS GESTURE RESOLVES ==================================
    // The drop consumed the gesture, so the withheld re-read is released — which is what makes the
    // loud "re-make your edit against what is there now" actionable instead of leaving the human
    // staring at a value that is no longer on disk.
    SMOKE_CHECK(pump_until("window B's released re-read to land", 30,
                           [&]
                           {
                               bool overridden = false;
                               return panels::inspector_field_value(b_surfaces.builtin, kFovPointer,
                                                                    overridden) == kRaceValue;
                           }),
                "window B's Inspector caught up to window A's racing value once its gesture ended");
    {
        const panels::InspectorObservation b = panels::observe_inspector(b_surfaces.builtin);
        SMOKE_CHECK(!b.refresh_deferred, "nothing is owed any more");
        SMOKE_CHECK(b.base_raw_hash != b_base, "…and window B now guards LIVE state");
    }

    // Nothing was refused on EITHER router across the whole scenario: with N windows the
    // deny-by-default router is N routers, and a surface installed on only one of them is a class of
    // bug that did not exist before multi-window.
    SMOKE_CHECK(primary_bridge.refused() == 0, "window A's bridge refused nothing");
    SMOKE_CHECK(created_bridge != nullptr && created_bridge->refused() == 0,
                "window B's bridge refused nothing");
    SMOKE_CHECK(shell::cef::frames_dropped_without_sink() == 0,
                "no live window ever lost an OSR frame to an unbound sink");

    // --- teardown, in the ONE order that is safe --------------------------------------------------
    // Clear every bag's non-owning client view BEFORE the daemon link is torn down: the teardown that
    // follows still PUMPS (manager.shutdown() closes the browsers, which drives CEF), so a renderer
    // message queued before exit can still reach a panel provider and issue a write. Unbinding first
    // is what makes that write refuse instead of calling a destroyed Client.
    for (WindowSurfaces* surfaces : fed)
    {
        panels::bind_write_client(surfaces->builtin, nullptr);
        if (surfaces->builtin.session != nullptr)
        {
            panels::bind_session_client(*surfaces->builtin.session, nullptr);
        }
    }
    // Then CEF, in the CE #319 order: manager.shutdown() closes every browser in ONE all-closing
    // drain; shell::cef::shutdown() finishes CEF's own teardown while every router is still alive;
    // only then does `manager` unwind, at the end of main, after CEF is gone.
    manager.shutdown();
    shell::cef::shutdown();
    // Finally the daemon: owned and sole-client, so this is a clean in-band `shutdown` verb.
    lifecycle.shutdown_at_exit();
    std::filesystem::remove_all(project, ec);

    if (g_failures != 0)
    {
        std::fprintf(stderr,
                     "[editor-cef-smoke-shell-inspector-fanout] FAILED with %d assertion "
                     "failure(s)\n",
                     g_failures);
        return finish(1);
    }
    std::printf("[editor-cef-smoke-shell-inspector-fanout] PASS: a DOM edit in window A reached "
                "window B's rendered Inspector through a real daemon; the mid-gesture settle was "
                "deferred and the L-30 guard held\n");
    return finish(0);
}
