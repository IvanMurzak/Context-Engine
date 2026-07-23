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

#pragma once

#include "context/editor/shell/browser.h"
#include "context/editor/shell/dpi.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/render/rhi.h"

#include <filesystem>
#include <memory>
#include <string>

namespace context::editor::shell::cef
{

// CEF subprocess re-entry. On Windows/Linux the renderer, GPU and utility processes re-exec THIS
// binary; this returns >= 0 in such a process, and the caller must return that value IMMEDIATELY —
// before parsing arguments or touching the filesystem, or every subprocess pays for it.
[[nodiscard]] int execute_subprocess(int argc, char** argv);

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
};

// Initialize CEF (once per process) and create the windowed-OSR browser. Returns nullptr plus
// `error` when CEF could not initialize or the browser could not be created.
[[nodiscard]] std::unique_ptr<IBrowserHost> make_cef_browser_host(const CefShellOptions& options,
                                                                  std::string& error);

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
