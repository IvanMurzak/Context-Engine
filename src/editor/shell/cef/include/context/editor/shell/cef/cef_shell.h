// The windowed-OSR CEF binding for the Shell (design 03 §1, §3) — the ONE piece of e04 that cannot
// build locally.
//
// It implements `IBrowserHost` (browser.h) over a real CEF browser, so everything above it — the
// compositor, the layer stack, the PET_POPUP layer, input arbitration, the owner loop — stays
// CEF-free and unit-tested on all three OS legs of the default `build` matrix. This header names no
// CEF type, so the app can include it unconditionally.
//
// WINDOWED-OSR: the browser renders off-screen (OnPaint delivers pixels the compositor composites)
// but the native window is passed as the device-context OWNER. That is what gives the browser a
// correct screen/DPI context and lets IME and native menus resolve against a real window, which a
// SetAsWindowless(0) browser cannot do.
//
// PER THE OWNER RULING OF 2026-07-19 the Windows accelerated (`OnAcceleratedPaint` -> shared-handle
// import) path is NOT implemented: stock wgpu-native exposes no external-texture import and a
// patched fork was rejected. The seam is still WIRED — `CefShellOptions::accelerated_osr` feeds
// e03's `OsrImportOptions`, whose per-platform policy decides — so restoring the path once
// gfx-rs/wgpu-native#621 lands is a policy flip plus a backend implementation, with nothing here to
// re-architect. `shared_texture_enabled` is deliberately left at its default (off).
//
// NEVER `SendExternalBeginFrame` (L-41 / cef#4033): CEF-internal pacing only.

// M9 e05c ADDS TWO THINGS HERE, both still CEF-type-free in this header: the `context-editor://`
// app scheme (assets served from `app_asset_root`, never `file://`) and the privileged IPC bridge
// (`bridge`, whose routing + validation live in the CEF-free `BridgeRouter`). The binding is a thin
// translator on both — a CefRequest becomes an `AppAssetResolver::resolve` call, and a
// CefMessageRouter query becomes a `BridgeRouter::dispatch` call. That is deliberate: this TU is
// the one the local dev gate cannot build, so it holds as little judgement as possible.

// M9 e13a-1 ADDS A SECOND SCHEME on the same terms: `context-ext://<package-id>/…`, the
// per-package origin third-party panels are served from (design 04 §5 / 08 §1-§2). It is registered
// in EVERY process pinned to `STANDARD|SECURE|CORS_ENABLED`, and its resource handler is installed
// UNCONDITIONALLY — with no package mounted it refuses every request, which is the point. All the
// judgement lives in the CEF-free `ExtAssetResolver` (ext_scheme.h). What e13a-1 does NOT add is
// any of the panel system above it: the iframe host, the MessageChannel bridge, the capability
// model and the scaffold are e13a-2 and e13b-f.

#pragma once

#include "context/editor/shell/browser.h"
#include "context/editor/shell/dpi.h"
#include "context/editor/shell/ext_scheme.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/render/rhi.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::shell::cef
{

// CEF subprocess re-entry. On Windows/Linux the renderer, GPU and utility processes re-exec THIS
// binary; this returns >= 0 in such a process, and the caller must return that value IMMEDIATELY —
// before parsing arguments or touching the filesystem, or every subprocess pays for it.
//
// On macOS it always returns -1: there is no self-re-exec model there, and every subprocess type runs
// from its own helper bundle through `execute_helper_process` below.
[[nodiscard]] int execute_subprocess(int argc, char** argv);

// --- macOS: the per-process-type helper bundle entry point (M9 e12c-1) ---------------------------
//
// The `main` of every "<App> Helper*.app" bundle: load the Chromium Embedded Framework for a NON-main
// process (`CefScopedLibraryLoader::LoadInHelper`), then hand control to `CefExecuteProcess` with the
// Shell's own `CefApp`. Returns the value the helper's `main` must return. The whole body is one call
// (`src/shell_helper_mac.cpp`) so a helper bundle needs no CEF knowledge of its own.
//
// ⚠ THIS IS WHY IT EXISTS AT ALL, and why it is not simply `CefExecuteProcess(args, nullptr, nullptr)`.
// The two older helpers in this repo — `src/editor/cef/src/cef_boot_smoke_helper_mac.cpp` and
// `src/editor/gui/host/src/editor_host_helper_mac.cpp` — pass a NULL `CefApp`, which is legal ONLY
// because neither of their apps has renderer-process duties. The Shell's app object does, three of
// them, and all three live in the RENDERER process where a null app means they never run:
//
//   * `OnRegisterCustomSchemes` for BOTH `context-editor://` and `context-ext://`. The renderer is
//     where origin comparisons, CSP evaluation and module loading happen, so a scheme registered only
//     browser-side leaves editor-core's documents (and every package panel's) on an OPAQUE origin —
//     the "works until you load a module" failure.
//   * `OnContextCreated` / `OnContextReleased` / `OnProcessMessageReceived`, the renderer half of the
//     message router. This is what INJECTS `contextEditorQuery` into a frame; without it the bridge
//     handshake cannot happen and every live smoke fails with a browser that booted fine.
//
// The app object is process-global inside the binding (the same one the browser process creates), so
// nothing here is a second configuration to keep in sync — that is the point of routing the helper
// through this function instead of letting each helper build its own.
//
// NON-macOS: returns 1 after a diagnostic. There is no helper-bundle process model on Windows/Linux
// (they re-exec the main binary — see `execute_subprocess`), so reaching this is a build-graph
// mistake, and 1 is deliberately a FAILING exit code: a helper that silently returned 0 would let a
// mis-wired subprocess look like a clean one.
[[nodiscard]] int execute_helper_process(int argc, char** argv);

struct CefShellOptions
{
    // The native window that OWNS the browser's device context (HWND on Windows). Null is allowed
    // and means a fully windowless browser — the honest configuration on a headless host.
    void* native_window = nullptr;
    // The view size in LOGICAL (DIP) pixels plus the monitor scale — CEF's GetViewRect and
    // GetScreenInfo::device_scale_factor. Physical pixels here lay the document out at the wrong size.
    render::Extent2D logical_size{1280, 800};
    DpiScale dpi;
    std::string url = "about:blank";
    // Where CEF keeps its cache. Empty means a per-PID temp dir — Chromium takes a process-singleton
    // lock on this directory, so two editors sharing one would deadlock on boot.
    std::filesystem::path cache_root;
    // DevTools via a hosted window or the remote-debugging port. DEV LOOP ONLY (review B-F11): a
    // naive OSR pass-through does not display, and an open debugging port is a shipped-editor
    // security hole. 0 disables it.
    bool devtools_enabled = false;
    int remote_debugging_port = 0;
    // Ask CEF for the accelerated (shared-texture) OSR path. Default OFF and, on Windows,
    // unreachable BY POLICY per the owner ruling above — see the file header.
    bool accelerated_osr = false;
    // Frames per second CEF paints at when windowless. 60 is CEF's own default; the compositor
    // decouples the engine's present rate from it (measured in the spike).
    int windowless_frame_rate = 60;

    // Opt-in Chromium/CEF verbose logging for the WHOLE process tree — LOGSEVERITY_VERBOSE plus
    // `--enable-logging=stderr --v=1`, which CEF propagates to the renderer/GPU/utility subprocesses
    // it spawns. OFF by default (the shipping editor stays at LOGSEVERITY_WARNING and quiet). It
    // exists for the un-locally-reproducible CEF-only smokes (e.g. the restart smoke), where the only
    // failure signal is a process exit code and the actual fault lives inside a subprocess whose
    // errors are otherwise never surfaced — turning it on makes Chromium name the cause on stderr.
    bool verbose_logging = false;

    // Isolate Chromium's OSCrypt profile-encryption key from the MACHINE keychain, via Chromium's
    // own `--use-mock-keychain` test switch. OFF by default — a shipped editor keeps the real OS
    // key store. Every SMOKE turns it ON, and that is not a convenience: on macOS it is the
    // difference between a test that finishes and one that hangs forever (issue #437).
    //
    // WHY (measured, CE #437). Chromium reads the `"<product> Safe Storage"` generic-password
    // keychain item to derive the profile encryption key, on a base::ThreadPool task posted
    // BLOCK_SHUTDOWN. macOS binds that item's ACL to the CREATING executable's code signature
    // (cdhash for an ad-hoc-signed local build), so ANY other binary — including the SAME target
    // after a rebuild, since the cdhash changes — is off the ACL, and securityd raises a modal
    // SecurityAgent authorization prompt. `SecItemCopyMatching` then blocks inside
    // `SecurityServer::ClientSession::decrypt` until a human answers it. Nobody answers on an
    // unattended host, so the BLOCK_SHUTDOWN task never completes and `CefShutdown()` — which waits
    // on the ThreadPool shutdown event — never returns. The smoke has already printed its verdict by
    // then, which is exactly why #437 reads as "passes, then hangs in teardown".
    //
    // This is the SECOND piece of shared machine state a CEF smoke must isolate; `cache_root` above
    // is the first (the Chromium process singleton). Same shape, same reason: a test may not depend
    // on machine-global state another process — or another build of itself — also owns.
    bool use_mock_keychain = false;

    // --- e05c: the app scheme + the privileged bridge --------------------------------------------

    // editor-core's built asset root, served over `context-editor://app/…` (design 04 §1). Empty
    // disables the scheme entirely — the browser then has no app to load, which is the honest state
    // of a build whose web assets were not produced, NOT a reason to fall back to `file://`. There
    // is deliberately no file-URL path anywhere in this binding.
    std::filesystem::path app_asset_root;

    // The privileged native<->JS channel (design 04 §1 / 08 §1). Null disables the bridge: the
    // query function is not injected and editor-core reports itself detached.
    //
    // NON-OWNING, and the router must OUTLIVE every browser created from these options — the CEF
    // message-router handler holds this pointer for the browser's whole life. `BridgeRouter` is
    // non-copyable AND non-movable precisely so a caller cannot relocate one out from under it.
    //
    // The Shell registers its handlers and calls `protect_secret()` with the attach token BEFORE
    // handing the router over; the token never appears in these options and never reaches CEF.
    BridgeRouter* bridge = nullptr;

    // --- e13a-1: the `context-ext://` extension scheme ---------------------------------------------

    // The third-party packages whose assets may be served over `context-ext://<package-id>/…`
    // (design 04 §5). Each entry is one package id and the directory its bundled assets live in.
    //
    // EMPTY IS THE DEFAULT AND IS FULLY SUPPORTED: the scheme is registered in every process and its
    // handler is installed REGARDLESS — an `ExtAssetResolver` with no mounts refuses every request,
    // which is the deny-by-default posture this scheme exists to have. Nothing here is a way to turn
    // the boundary off.
    //
    // A mount that is refused (invalid id, missing root, a root that overlaps another package's, or a
    // root that fails the PROVENANCE check against `ext_store_root` — see ext_scheme.h) is REPORTED
    // on stderr and skipped; the browser still boots and that one origin serves 403. There is
    // deliberately no `file://` fallback for panels either.
    //
    // ✅ A REAL PRODUCER EXISTS SINCE M9 e13c-3 — `editor_main.cpp` fills this from
    // `package_mounts(scan_package_store(package_store_root()))`, so what a user has installed in
    // `~/.context/packages` is what gets mounted. The field is no longer a declared-only seam, which
    // is why `ext_store_root` below is now REQUIRED alongside it rather than implied.
    //
    // ⚠ PROCESS-GLOBAL, FIRST CALL WINS. The resolver and its scheme-handler factory are registered
    // once per process (CEF holds the factory until CefShutdown), so only the FIRST
    // make_cef_browser_host() call's list is honoured — a second window cannot mount a different
    // package set, and one that tries has its list ignored with a stderr report rather than
    // silently. This is therefore a PROCESS-level option that happens to live on a per-window
    // struct; e13b, which owns the install path, is where a real per-process mount table belongs.
    std::vector<ExtPackageMount> ext_packages;

    // The PACKAGE STORE every entry in `ext_packages` must have come from (M9 e13c-3) — the canonical
    // `~/.context/packages` in production (`package_store_root()`), or a smoke's own temp fixture
    // directory, which IS that smoke's package store.
    //
    // ⚠ EMPTY REFUSES EVERY MOUNT, and that is the point rather than a rough edge. `mount()` takes
    // this as a REQUIRED argument and answers `package.store_root_unset` for an empty one, so a
    // caller who populates `ext_packages` and forgets this field gets every package refused with a
    // named reason on stderr — never every package admitted with its provenance unchecked. That
    // direction is what discharges e13a-1's E13B obligation at the API rather than by convention:
    // there is no spelling of "mount these packages" that does not also state where they came from.
    std::filesystem::path ext_store_root;
};

// Initialize CEF (once per process) and create the windowed-OSR browser. Returns nullptr plus
// `error` when CEF could not initialize or the browser could not be created.
[[nodiscard]] std::unique_ptr<IBrowserHost> make_cef_browser_host(const CefShellOptions& options,
                                                                  std::string& error);

// --- the containment counters (M9 e10a) ----------------------------------------------------------
//
// `OnBeforePopup` suppression (03 §1) is a SECURITY CONTAINMENT BOUNDARY, not a cosmetic one: no
// `window.open` from renderer content may ever produce a native window the Shell does not manage
// and composite. That claim is only testable if the binding says what actually happened, so it
// counts both halves — process-wide, because CEF's browser creation is process-wide:
//
//   * `browsers_created()` — how many CEF browsers this process has created, counted in
//     `OnAfterCreated`. It is the NEGATIVE half: after a `window.open`, this must be UNCHANGED.
//   * `popups_suppressed()` — how many popup requests `OnBeforePopup` refused. It is the POSITIVE
//     half, and it is what makes the assertion non-vacuous: without it a test would pass equally
//     well if the popup never reached CEF at all (blocked upstream, or the script never ran), which
//     proves nothing about OUR boundary.
//
// Both are plain counters read from the owner thread after pumping, never a control surface.
[[nodiscard]] int browsers_created();
[[nodiscard]] int popups_suppressed();

// How many OSR frames were delivered to a LIVE, already-bound browser whose frame sink was not
// bound at that moment — i.e. frames that were silently thrown away.
//
// This is the tripwire for the N-window frame-delivery defect e10a shipped first and CI caught:
// `CefDoMessageLoopWork()` is PROCESS-WIDE, so one window's pump dispatches every OTHER window's
// pending `OnPaint`. With the sink bound only for the duration of a window's own pump call, the
// secondary windows' frames landed on the floor and those windows composited nothing — for the
// entire 30-second boot window, deterministically, on both CI legs, while their bridges and
// handshakes worked perfectly (which is what made the symptom read as "the window is blank"
// rather than "frames are being dropped").
//
// The binding now spans the pump (`IBrowserHost::pump` in browser.h), which makes a nonzero value
// here UNREACHABLE — and that is the point: this is a REGRESSION TRIPWIRE, not an independent
// measurement of frame delivery. Undo the binding and the count goes nonzero immediately, so the
// multiwindow smoke's assertion names the cause instead of leaving a blank window to be diagnosed
// from CI logs. Paints before the owner's first pump and from `close()` onward are excluded —
// neither is a loss.
[[nodiscard]] int frames_dropped_without_sink();

// --- the extension-scheme request log (M9 e13a-2) ------------------------------------------------
//
// WHAT THE LIVE IFRAME SMOKE CAN ACTUALLY OBSERVE, and why it has to be this and not a frame handle.
// A third-party panel document is on its OWN opaque-origin sandbox: the Shell cannot read its DOM,
// editor-core cannot read it either, and e13a-2 ships no bridge into it by design. So "did the panel
// really load?" is answerable at exactly ONE place — the bytes the scheme handler served — and these
// two lists are that place.
//
// THE ASSERTION THEY MAKE POSSIBLE IS NOT "a request arrived". A blocked frame still fetches its
// top-level document (CSP `frame-ancestors` is evaluated on the RESPONSE), so counting one served
// URL proves nothing about rendering. What proves it is WHICH urls: a panel whose own `panel.css`
// and `panel.js` were requested is a panel whose document was PARSED, which a blocked frame's never
// is — and a panel whose ES module's own `import` resolved is one whose script actually RAN. That
// chain is what discharges e13a-1's two open hypotheses (ext_scheme.h) and what verifies the
// `frame-ancestors` tightening, so the smoke needs the URLs, not a count.
//
//   * `ext_served_urls()`  — URLs the resolver answered `ok` for, in order.
//   * `ext_refused_urls()` — URLs it REFUSED (any non-ok status), in order. The negative half, and
//     the one that keeps the positive from being vacuous: a smoke that also asks for an UNMOUNTED
//     package and finds it here has proven the deny-by-default posture is live in the browser, not
//     merely in the unit suite.
//
// THREADING — DIFFERENT FROM THE COUNTERS ABOVE, deliberately so. `browsers_created()` and friends
// are written on the CEF UI thread, which IS the owner thread here, so they are plain ints. A scheme
// handler's `Open` runs on the CEF IO THREAD, so this log genuinely is cross-thread and takes a
// mutex. Do not "harmonize" the two into one style: the lock here is load-bearing and the absence of
// one there is a documented statement about the design.
//
// BOUNDED. The requester is untrusted by construction and controls the URL, so an unbounded log is
// an unbounded attacker-chosen allocation. Entries past the cap are dropped (the smoke needs the
// first handful), and each URL is truncated exactly as the refusal log line is.
[[nodiscard]] std::vector<std::string> ext_served_urls();
[[nodiscard]] std::vector<std::string> ext_refused_urls();

// Shut CEF down. Call ONCE, after every browser host has been closed. Idempotent.
//
// LIFETIME INVARIANT — `shutdown()` must run BEFORE the `bridge` router (and everything its
// handlers capture: the PanelHost, the panel models, the editor-state bridge, the window manager)
// is destroyed. Closing the browser host is NOT enough: CEF keeps browser/frame state alive past
// `CloseBrowser` and finishes tearing it down INSIDE `CefShutdown()`, still dispatching frame work
// to the client — whose message-router handler holds the non-owning `CefShellOptions::bridge`
// pointer above. Destroying the router first therefore leaves that pointer dangling across
// `CefShutdown()`, which on the Session-0 Windows CI runner faults with an ACCESS_VIOLATION inside
// CEF's own global teardown (CE #319). `editor_main.cpp` and the single-boot smokes satisfy this by
// calling `manager.shutdown()` then `shutdown()` while every bridge local is still in scope; a
// helper that owns a session's bridge surfaces must call `shutdown()` before it returns.
void shutdown();

} // namespace context::editor::shell::cef
