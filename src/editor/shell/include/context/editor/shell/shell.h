// The Shell itself (design 03 §1) — `WindowManager` owns N `EditorWindow`s, and each `EditorWindow`
// binds one native window to one OSR browser, one compositor, and one input arbiter.
//
// THE OWNER LOOP. Production runs `multi_threaded_message_loop=false` with an INTEGRATED pump on the
// shell's main thread: one thread drains the OS queue, drives the browser's message work, and
// composites. The spike's "prod = multi-threaded + mutex" caveat is REJECTED by the design in favour
// of this single-threaded owner loop — simpler invariants, and the compositor already decouples
// engine frame rate from CEF's 60 Hz, which was the only thing the extra thread bought.
//
// `pump_once` is therefore the whole loop body, and it takes the clock as an argument rather than
// reading one. That is what makes the entire shell lifecycle — resize, DPI change, focus, input
// round-trip, popup, placement persistence, teardown — a deterministic ctest instead of something
// only a human at a real window can observe.
//
// THE SHELL IS AN ORDINARY CLIENT (D10). It reaches the daemon through the published `context_client`
// SDK, never by linking the kernel's own modules, and it MUST present the D20 attach token —
// enforcement has been on since e02. `guard_shell_attach` refuses to even ATTEMPT an unauthenticated
// attach, so a missing token is reported as what it is rather than surfacing later as an opaque
// `attach.denied` that reads like a transport fault.

#pragma once

#include "context/editor/client/client.h"
#include "context/editor/shell/browser.h"
#include "context/editor/shell/compositor.h"
#include "context/editor/shell/editor_state.h"
#include "context/editor/shell/input.h"
#include "context/editor/shell/window.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::shell
{

// ------------------------------------------------------------------------- the daemon attach (D10)

// The scopes the Shell needs: it reads and queries, it writes authored files through the inspector's
// L-30 commit path, and it drives play sessions. Named once so a caller cannot quietly widen them.
inline constexpr const char* kShellScope = "read,write,session";

// Refuse an attach that carries no token (see the header). `discovered_token` is what
// `Client::connect_to_project` read out of `.editor/instance.json`; `options.token` overrides it when
// non-empty. Returns false + `reason` when neither yields a token.
[[nodiscard]] bool guard_shell_attach(const client::AttachOptions& options,
                                      const std::string& discovered_token, std::string& reason);

// The attach options the Shell uses. Kept as a function so the scope + capability set has ONE
// definition rather than one per call site.
[[nodiscard]] client::AttachOptions make_shell_attach_options(std::string token = {});

struct DaemonAttach
{
    std::unique_ptr<client::Client> client;
    bool attached = false;
    // Empty on success. Populated for every failure mode: no discoverable daemon, an unauthenticated
    // attach refused before it was attempted, a daemon refusal, a transport fault.
    std::string error;
    // The daemon's catalog code when it refused (`attach.denied`, …), empty otherwise.
    std::string error_code;
};

// Discover + connect + attach, as an ordinary authenticated client. Never throws; a failure is a
// reported state (the editor opens read-only and retries) rather than a boot failure.
[[nodiscard]] DaemonAttach attach_to_project(const std::filesystem::path& project_root,
                                             int timeout_ms = 5000);

// ------------------------------------------------------------------------------------ EditorWindow

struct EditorWindowConfig
{
    // Index into `EditorState::windows` — window 0 hosts the app menu + welcome screen (D13).
    std::size_t state_index = 0;
    CompositorConfig compositor;
    // How often the loop re-reads the OS window placement to persist it. Placement is polled rather
    // than derived from move/resize events alone because a window can also be moved by the OS
    // (a monitor being unplugged) with no event the Shell subscribes to.
    std::uint64_t placement_poll_us = 250'000;
};

// One window: native handle + swapchain + one browser + compositor + input binding (03 §1).
class EditorWindow
{
public:
    EditorWindow(std::unique_ptr<IWindowBackend> backend, std::unique_ptr<IBrowserHost> browser,
                 const EditorWindowConfig& config);

    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // Attach the GPU present path over `rhi`, or fall back to the CPU path (C-F2) when no adapter
    // can present to this window. Returns the path actually taken — the caller REPORTS it rather
    // than assuming, because "the editor is running on the CPU path" is exactly what a user with a
    // sluggish editor needs to be able to find out.
    PresentPath attach_present(render::IRhi& rhi);
    // Take the CPU path unconditionally (a GPU-less boot, or after an unrecoverable device loss).
    void attach_cpu_present();

    // One iteration of the owner loop: drain OS events, arbitrate + dispatch input, drive the
    // browser, composite. Returns false once the window is gone.
    bool pump_once(std::uint64_t now_us);

    [[nodiscard]] IWindowBackend& backend() { return *backend_; }
    [[nodiscard]] IBrowserHost& browser() { return *browser_; }
    [[nodiscard]] WindowCompositor& compositor() { return compositor_; }
    [[nodiscard]] InputArbiter& input() { return input_; }
    [[nodiscard]] std::size_t state_index() const { return config_.state_index; }
    [[nodiscard]] bool alive() const { return alive_; }
    [[nodiscard]] const std::string& diagnostic() const { return diagnostic_; }
    // True once the placement changed since the last time the manager persisted it.
    [[nodiscard]] bool placement_dirty() const { return placement_dirty_; }
    [[nodiscard]] const WindowPlacement& last_placement() const { return last_placement_; }
    void clear_placement_dirty() { placement_dirty_ = false; }

    void close();

private:
    void handle_event(const ShellEvent& event, std::uint64_t now_us);
    void sync_browser_size();
    void poll_placement(std::uint64_t now_us);

    std::unique_ptr<IWindowBackend> backend_;
    std::unique_ptr<IBrowserHost> browser_;
    EditorWindowConfig config_;
    WindowCompositor compositor_;
    InputArbiter input_;
    std::unique_ptr<render::IDevice> device_;
    std::unique_ptr<render::ISurface> surface_;
    WindowPlacement last_placement_;
    std::uint64_t last_placement_poll_us_ = 0;
    std::string diagnostic_;
    bool placement_dirty_ = false;
    bool alive_ = true;
    bool browser_size_synced_ = false;
};

// ------------------------------------------------------------------------------------ WindowManager

// Owns the windows and the ONE editor-state store they persist placement through (03 §1: the Shell
// is `.editor/editor-state.json`'s single writer — one store, not one per window, or two windows
// would race each other on the same file).
class WindowManager
{
public:
    explicit WindowManager(std::filesystem::path project_root);

    // Adopt a window. Its remembered placement (if any) is applied before the first frame.
    EditorWindow& add(std::unique_ptr<EditorWindow> window);

    // One pass over every window: pump each, persist any placement change, drop dead ones. Returns
    // false when no window is left — the loop's termination condition.
    bool pump_once(std::uint64_t now_us);

    // Flush pending session state and close every window. Idempotent.
    void shutdown();

    [[nodiscard]] std::size_t window_count() const { return windows_.size(); }
    [[nodiscard]] EditorWindow* window(std::size_t index);
    [[nodiscard]] EditorStateStore& state_store() { return store_; }
    [[nodiscard]] int pumps() const { return pumps_; }

private:
    std::filesystem::path project_root_;
    EditorStateStore store_;
    std::vector<std::unique_ptr<EditorWindow>> windows_;
    int pumps_ = 0;
    bool shut_down_ = false;
};

} // namespace context::editor::shell
