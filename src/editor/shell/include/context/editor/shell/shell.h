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
#include "context/editor/shell/viewport_binding.h"
#include "context/editor/shell/window.h"
#include "context/editor/shell/window_registry.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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
    // The Scene viewport producer (editor-UX e3, D7). Exposed so the composition root can hydrate its
    // cameras from `editor.cameras-get` and push local moves back through `editor.camera-set`, and so
    // the panel feed can read `adapter_available()` for the degraded summary — all of which are the
    // APP's wiring, not the window's, exactly like the panel host itself.
    [[nodiscard]] ViewportBinding& viewports() { return viewports_; }
    [[nodiscard]] const ViewportBinding& viewports() const { return viewports_; }
    // The scene the viewports draw. EMPTY until a daemon scene-data read exists (viewport_binding.h
    // § SCENE DATA, HONESTLY): every viewport renders the D5 grid and nothing else today, which is
    // the honest state rather than a stub. Handed out by reference so the day that read lands, the
    // feed fills THIS snapshot and the producer needs no new argument.
    [[nodiscard]] render::RenderSnapshot& viewport_scene() { return viewport_scene_; }
    [[nodiscard]] std::size_t state_index() const { return config_.state_index; }
    [[nodiscard]] bool alive() const { return alive_; }
    // Whether the OS last reported this window focused (a1: `chrome.state.focused`). False until the
    // first focus_gained arrives — the honest answer for a window the OS has said nothing about,
    // which is also every headless window.
    [[nodiscard]] bool focused() const { return focused_; }
    [[nodiscard]] const std::string& diagnostic() const { return diagnostic_; }

    // --- b1: the OSR drag protocol (D11; osr_drag.h, docs/shell.md § 16) --------------------------
    //
    // The window is where the drag lives, because the window is what owns the two things a drag
    // needs and nothing else has: the OS POINTER STREAM (the browser cannot see it — it is
    // off-screen) and the VIEW RECT to hit-test that stream against. Exposed read-only so a test and
    // the live smoke can assert the SAME session the shipping owner loop drives, rather than a
    // parallel one built for the assertion.
    [[nodiscard]] const OsrDragSession& drag() const { return drag_; }
    // True once the placement changed since the last time the manager persisted it.
    [[nodiscard]] bool placement_dirty() const { return placement_dirty_; }
    [[nodiscard]] const WindowPlacement& last_placement() const { return last_placement_; }
    void clear_placement_dirty() { placement_dirty_ = false; }
    // Re-read `last_placement()` from the backend NOW, without marking it dirty. For a caller that
    // just mutated the backend's placement behind the poll's back — the manager's adoption-time
    // restore (add_session applies the remembered placement AFTER this window's constructor cached
    // the pre-restore one) — so every consumer of `last_placement()` (the persistence write, and
    // a1's chrome-fact flip compare) starts from the same truth the backend reports, instead of a
    // stale cache that disagrees until the first poll interval elapses.
    void sync_placement_baseline() { last_placement_ = backend_->placement(); }

    // Single-shot close: ask the browser to close AND drain it (the browser host's own close), then
    // detach the compositor and the backend. Correct for a window torn down on its own; the manager's
    // N-window teardown uses the split begin_close()/browser_closed()/finish_close() below so the CEF
    // drain is shared across all windows (browser.h § teardown; the e10a Windows `!in_dtor_` fix).
    void close();

    // --- split teardown, driven by WindowManager for N windows -----------------------------------
    // Phase 1: ask the browser to close (unbind its sink + CloseBrowser), WITHOUT draining the loop.
    void begin_close();
    // Has the browser finished closing (its OnBeforeClose ran)? True also when there is no browser.
    [[nodiscard]] bool browser_closed() const;
    // The pump-less browser seam, so the manager can drive the shared teardown loop through a window.
    [[nodiscard]] IBrowserHost* browser_or_null() { return browser_.get(); }
    // Retire mid-process: unbind the browser's frame sink so it stops painting, but do NOT close it —
    // its CEF teardown is deferred to the manager's shared all-closing `shutdown()` drain
    // (browser.h § IBrowserHost::detach; the e10a `!in_dtor_` fix). Paired with `finish_close()`, which
    // then releases the OS/GPU resources while the browser host stays alive in the graveyard.
    void detach_browser();
    // Phase 3: the non-CEF teardown (detach the compositor + the backend, mark dead), run AFTER the
    // shared drain has confirmed the browser closed. The browser is NOT re-closed here.
    void finish_close();

private:
    // --- b1: the browser's drag observer -----------------------------------------------------------
    //
    // A MEMBER ADAPTER rather than a base of `EditorWindow`, deliberately: the window's public
    // surface is what the app, the smokes and the bridge handlers all program against, and growing
    // it by an interface CEF calls INTO would let any of them call `on_start_dragging` — which is a
    // report from the renderer, never a command. Non-copyable for the same reason `EditorWindow` is:
    // it holds a back-pointer to the one window that owns it.
    class DragObserver final : public IBrowserDragObserver
    {
    public:
        explicit DragObserver(EditorWindow& owner) : owner_(&owner) {}

        [[nodiscard]] bool on_start_dragging(DragOperationMask allowed,
                                             PointI start_view_dip) override;
        void on_update_drag_cursor(DragOperation operation) override;

    private:
        EditorWindow* owner_ = nullptr;
    };

    void handle_event(const ShellEvent& event, std::uint64_t now_us);
    // Feed one pointer sample into the live drag and apply whatever injections it emits. Called
    // INSTEAD of `send_pointer` for the whole duration of a drag — see the pointer arm in
    // `handle_event` for why the drag REPLACES the mouse stream rather than riding beside it.
    void drive_drag(const PointerDispatch& dispatch, const PointerEvent& event);
    // Apply an emitted sequence to the browser, in order, and reset the drag cursor once the
    // session has ended. The ONE place injections reach `IBrowserHost`.
    void apply_drag(const OsrDragInjections& injections);
    // End a live drag because the WINDOW is going away or losing the gesture. `inject` is false on
    // the teardown paths, where the browser is already closing and there is nothing left to tell.
    void end_drag(OsrDragEndReason reason, bool inject);
    // Push a feedback cursor to the backend, ONCE PER CHANGE — the single funnel every drag-cursor
    // write goes through (`on_update_drag_cursor`, `apply_drag`'s end-of-drag reset, `end_drag`'s).
    //
    // The guard is here rather than in the three backends because it is the same guard in all
    // three, and only X11 could ever have justified writing its own (a server resource per change).
    // `UpdateDragCursor` fires as often as CEF answers an injected `DragTargetDragOver` — up to one
    // per pointer sample — while the operation it reports is near-constant for a whole gesture, so
    // without this every sample would re-issue a window-server call (`[NSCursor set]`) or a USER32
    // one on the single thread that also runs `CefDoMessageLoopWork`.
    void push_drag_cursor(DragCursor cursor);
    void sync_browser_size();
    // Push the window's CLIENT origin on screen down to the browser (a1) — the half of the OSR
    // geometry contract `sync_browser_size` does not carry. Deliberately separate: a move is not a
    // resize, and folding it into `sync_browser_size` would fire CEF's `WasResized()` (a re-layout
    // + repaint) on every step of a window drag.
    //
    // COALESCED, never called straight from the event arms: they set `origin_dirty_` and `pump_once`
    // pushes at most once per iteration (see the flag).
    void sync_browser_origin();
    void poll_placement(std::uint64_t now_us);

    // b1: DECLARED BEFORE `browser_` ON PURPOSE — members are destroyed in reverse declaration
    // order, so anything declared AFTER the host would be gone while `~CefBrowserHostImpl` still
    // runs its close drain. The host also unbinds the observer in its own close path (the same
    // belt-and-braces the frame sink gets), so this ordering is the second of two independent
    // guarantees rather than the only one.
    OsrDragSession drag_;
    DragObserver drag_observer_{*this};
    // What `push_drag_cursor` last pushed. `none` is the backends' own initial state, so starting
    // here means a drag that never displayed a cursor also never issues a reset for one.
    DragCursor last_drag_cursor_ = DragCursor::none;
    std::unique_ptr<IWindowBackend> backend_;
    std::unique_ptr<IBrowserHost> browser_;
    EditorWindowConfig config_;
    WindowCompositor compositor_;
    InputArbiter input_;
    std::unique_ptr<render::IDevice> device_;
    std::unique_ptr<render::ISurface> surface_;
    // e3: DECLARED AFTER `device_` ON PURPOSE. Members destroy in reverse declaration order, and the
    // binding owns per-viewport textures created ON that device — so it must be torn down while the
    // device is still alive. (The same ordering argument `drag_` above makes about the browser.)
    ViewportBinding viewports_;
    render::RenderSnapshot viewport_scene_;
    WindowPlacement last_placement_;
    // Reused across pumps rather than constructed per frame: this is drained every loop iteration,
    // so a local would malloc/free once per frame with input. The Win32 backend keeps its own
    // pending_ buffer for the same reason.
    std::vector<ShellEvent> events_;
    // The RegionMap generation last pushed down to the backend (b1: pump_once § the chrome-region
    // push). Starts at 0 — the map's own pre-first-publish generation — so an empty map is never
    // pushed at boot, and the first real publish is.
    std::uint64_t chrome_regions_pushed_generation_ = 0;
    // e3: the RegionMap generation the viewport layer stack was last built from — the seam input.h
    // reserved ("this is the seam e11's viewport-content damage path is expected to read") and that
    // nothing consumed until now. One integer compare per pump, mirroring the chrome push above.
    std::uint64_t viewport_regions_generation_ = 0;
    std::uint64_t last_placement_poll_us_ = 0;
    std::string diagnostic_;
    bool placement_dirty_ = false;
    // a1: "the client origin the browser holds is stale", set by the event arms that can move it
    // (`moved`, and `sync_browser_size` for the resize/DPI/boot paths) and consumed ONCE per pump.
    //
    // The gate belongs in the mechanism rather than in the count of call sites: `client_origin()`
    // reads the LIVE OS position at drain time, so every call within one drain returns the same
    // value, and nothing observes the intermediate pushes — CEF pulls the mapping through
    // `GetScreenPoint` during `browser_->pump()`, after the drain. A Win32 caption drag is a modal
    // `DefWindowProc` loop that blocks this thread (`win32_window.cpp` § set_chrome_regions), so its
    // whole gesture's `WM_MOVE`s queue into `pending_` and drain in ONE batch — hundreds of
    // identical `ClientToScreen` round-trips where one suffices. Mirrors `placement_dirty_` beside
    // it and the generation gate in `pump_once`.
    bool origin_dirty_ = false;
    bool focused_ = false;
    bool alive_ = true;
    bool browser_size_synced_ = false;
};

// ------------------------------------------------------------------------------------ WindowManager

// Owns the windows and the ONE editor-state store they persist placement through (03 §1: the Shell
// is `.editor/editor-state.json`'s single writer — one store, not one per window, or two windows
// would race each other on the same file).
//
// SINCE e10a IT IS ALSO THE REGISTRY (window_registry.h): windows are peers addressable by a minted
// `WindowId`, window 0 is primary, and one can be created or destroyed at RUNTIME through a bound
// factory. Two invariants make that safe, and both are asserted by
// `editor-shell-test_window_registry`:
//
//   * **A retired session — the WHOLE session, browser included — outlives `CefShutdown`.**
//     `destroy_window` DETACHES the window (unbinds its browser's frame sink so it stops painting, and
//     releases its OS/GPU resources) and MOVES the entire session — its still-OPEN browser host, its
//     bridge, its daemon client and its captured surfaces — into a graveyard emptied ONLY by
//     ~WindowManager. It deliberately does NOT close + drain the browser mid-process: with sibling
//     browsers live in the same process-wide message loop, driving one browser's CEF teardown re-enters
//     a sibling's and aborts on Windows (`!in_dtor_`, CE #319 generalised). Every retired browser is
//     instead closed in ONE shared, all-closing `shutdown()` drain — where no browser is live — and
//     torn down by CEF, then destroyed here after `shell::cef::shutdown()`. `pump_once` retires a window
//     that died on its own the same way. The app must therefore destroy the manager AFTER
//     `shell::cef::shutdown()`, which `editor_main.cpp` does by declaring it in the enclosing scope:
//     `CloseBrowser` returning is not proof CEF is done with the client.
//   * **Ids are never reused.** A stale id from a destroyed window resolves to nullptr forever
//     rather than silently addressing a different window — which is what e10b's cross-window moves
//     would otherwise do to a panel.
class WindowManager
{
public:
    explicit WindowManager(std::filesystem::path project_root);

    // The graveyard is emptied here — see the class note. Declared so the ordering rule has a
    // documented home rather than living only in a comment at a call site.
    ~WindowManager();

    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    // Adopt a window. Its remembered placement (if any) is applied before the first frame. The first
    // adopted window becomes `kPrimaryWindowId`.
    EditorWindow& add(std::unique_ptr<EditorWindow> window);

    // Adopt a window together with the per-window session objects the manager must OUTLIVE it with
    // (its bridge, its daemon client, the surfaces its bridge captured). This is what `add` cannot
    // express, and what every created window uses.
    WindowId add_session(std::unique_ptr<EditorWindow> window, WindowSessionParts&& session);

    // --- the registry (e10a, design 03 §1) --------------------------------------------------------

    // Bind how a window is built. Without one, `create_window` reports `no_factory` rather than
    // pretending: a build with no browser binding genuinely cannot make a second window.
    void bind_window_factory(WindowFactory factory);
    [[nodiscard]] bool has_window_factory() const { return factory_ != nullptr; }

    // The 03 §7 degradation seam: called ONCE per failed create, with the full report. e10b owns
    // what to do about it.
    void on_window_create_failed(WindowCreateFailureSink sink);

    // a1 (editor-window-chrome, target design 02 §1): called once per OBSERVED maximized flip, with
    // the window's id and its new state. The observation rides the placement poll the loop already
    // runs — pump_once compares each window's polled `last_placement().maximized` against the state
    // it last saw — so there is no new poll and no OS event subscription. The composition root binds
    // this to the chrome-fact relay (chrome_facts.h), which turns the flip into an `editor.ui` fact
    // in the window's mirror queue; unbound (every test/smoke that doesn't care), flips are still
    // tracked and simply reported to nobody.
    using ChromeMaximizedSink = std::function<void(WindowId, bool)>;
    void on_chrome_maximized(ChromeMaximizedSink sink);

    // Create a window on demand. `source` is the window the request came from (e10b's floating-group
    // home on failure). Never throws and never partially adopts: on any failure nothing is added,
    // the failure sink fires exactly once, and the registry stays usable.
    [[nodiscard]] WindowCreateResult create_window(const WindowSpec& spec,
                                                   WindowId source = kPrimaryWindowId);

    // Destroy one window by id. Refuses the primary (it hosts the menu/welcome screen — the app
    // closes it via `shutdown()`), and refuses an unknown id. Its session is retired, not freed.
    WindowDestroyResult destroy_window(WindowId id);

    // The `window.close` POLICY, in the ONE place that owns both halves (design 03's window-close
    // table): a SECONDARY window's ✕ destroys that window; the PRIMARY's ✕ is the app QUIT, because
    // window 0 hosts the app menu and the welcome screen. "It closes with the app, not on its own"
    // is only true if asking it to close actually closes the app — before this existed the primary's
    // ✕ dispatched `window.close`, hit `destroy_window`'s primary refusal, and did NOTHING at all
    // (the minimize/maximize buttons beside it worked, so the frame looked alive and the ✕ was dead).
    //
    // `destroy_window`'s refusal is UNCHANGED and deliberately so: it stays the honest answer for
    // anyone asking to destroy window 0 as a window, which is what makes this the only path that
    // can close it. An unknown id is `unknown_window` here too — a stale id must never quit the app.
    [[nodiscard]] WindowDestroyResult close_window(WindowId id);

    // Ask EVERY live window to close — the same ask the OS makes for Alt+F4, per backend (win32
    // posts itself WM_CLOSE, X11 a self-addressed WM_DELETE_WINDOW, cocoa records `close_requested`,
    // headless flips its alive flag). Nothing is destroyed HERE: each window dies on its own next
    // pump and `pump_once` retires it through the one path that already exists, so the owner loop
    // ends through its ordinary termination condition (`windows_` empty) and the whole teardown
    // sequence — state flush, `shutdown()`, `CefShutdown` — runs unchanged. Idempotent: a second
    // call simply re-asks whatever has not died yet.
    void request_quit();
    // Whether a quit has been asked for. Assertable state, not a diagnostic: a window created after
    // this is a window nobody will see.
    [[nodiscard]] bool quit_requested() const { return quit_requested_; }

    // The window carrying `id`, or nullptr. `window(kPrimaryWindowId)` is the primary.
    [[nodiscard]] EditorWindow* window(WindowId id);
    [[nodiscard]] std::vector<WindowId> window_ids() const;
    [[nodiscard]] bool is_primary(WindowId id) const { return id == kPrimaryWindowId; }

    // THIS window's `origin` — the e08a echo-suppression identity of its own wire connection, so N
    // windows are genuinely N origins. A session that owns a client answers from it; otherwise the
    // value bound by `set_window_origin` (which is how the primary reports the app-owned client's
    // id). 0 means "no identity", which e08a also spells "not attached".
    [[nodiscard]] std::uint64_t window_origin(WindowId id) const;
    void set_window_origin(WindowId id, std::uint64_t origin);
    // How many DISTINCT non-zero origins the live windows carry. Equal to the window count exactly
    // when every window has its own connection — the property e10d's cross-window drills need.
    [[nodiscard]] std::size_t distinct_origins() const;

    // One pass over every window: pump each, persist any placement change, retire dead ones. Returns
    // false when no window is left — the loop's termination condition.
    bool pump_once(std::uint64_t now_us);

    // Flush pending session state and close every window. Idempotent. Does NOT free the sessions —
    // see the class note.
    void shutdown();

    [[nodiscard]] std::size_t window_count() const { return windows_.size(); }
    [[nodiscard]] EditorStateStore& state_store() { return store_; }
    [[nodiscard]] int pumps() const { return pumps_; }

    // --- what it recorded (assertable state, not diagnostics) --------------------------------------
    [[nodiscard]] std::size_t retired_session_count() const { return retired_.size(); }
    [[nodiscard]] std::size_t create_failures() const { return create_failures_; }
    [[nodiscard]] const WindowCreateFailure* last_create_failure() const
    {
        return last_failure_.has_value() ? &*last_failure_ : nullptr;
    }
    [[nodiscard]] WindowId last_minted_id() const { return next_id_ == 0 ? kInvalidWindowId
                                                                        : next_id_ - 1; }

private:
    // One live window plus the session objects its browser can still reach. MEMBER ORDER IS
    // DESTRUCTION ORDER REVERSED and load-bearing (window_registry.h § LIFETIME RULE): `window` —
    // and with it the browser — is destroyed first, then the daemon client, then the bridge, then
    // the surfaces the bridge's handlers captured.
    struct WindowEntry
    {
        std::vector<std::shared_ptr<void>> surfaces;
        std::unique_ptr<BridgeRouter> bridge;
        std::unique_ptr<client::Client> daemon_client;
        std::unique_ptr<EditorWindow> window;
        WindowId id = kInvalidWindowId;
        std::uint64_t origin = 0;
        // The maximized state the chrome-fact sink last saw for this window (a1). Seeded from the
        // backend's real placement at adoption, so a window RESTORED maximized does not fire a
        // spurious boot-time flip — `chrome.state` carries the initial state; the sink carries
        // CHANGES only.
        bool last_maximized = false;
    };

    // A destroyed window's session, held until ~WindowManager. Same member order, same reason — and
    // now it carries the WINDOW itself (and with it the browser host), because a mid-process destroy
    // must NOT tear the browser down while sibling browsers are live (the e10a `!in_dtor_` abort). The
    // browser is left OPEN (only its sink detached), closed together with every other browser in the
    // shared `shutdown()` drain, and destroyed here, after `shell::cef::shutdown()` — the browser dies
    // first, then the daemon client, then the bridge, then the surfaces its handlers captured.
    struct RetiredSession
    {
        std::vector<std::shared_ptr<void>> surfaces;
        std::unique_ptr<BridgeRouter> bridge;
        std::unique_ptr<client::Client> daemon_client;
        std::unique_ptr<EditorWindow> window;
    };

    // Detach a window (unbind its browser's sink + release its OS/GPU resources) and move the WHOLE
    // session — the window with its still-OPEN browser host included — into the graveyard. The browser
    // is NOT closed here: mid-process, closing + draining it while sibling browsers are live is the
    // `!in_dtor_` re-entrancy (CE #319 generalised), so its CEF teardown is deferred to `shutdown()`'s
    // shared all-closing drain and freed only by ~WindowManager. The entry is left empty for the
    // caller to erase.
    void retire(WindowEntry& entry);
    // Pump the PROCESS-WIDE CEF teardown loop until `done()` or a bounded budget is spent. Pumps
    // through any one live window's browser (the loop drains every closing browser at once), so this
    // is the ONE shared drain — run ONLY from `shutdown()`, after EVERY browser (live and retired) has
    // been asked to close, so no browser's teardown ever runs while another is still live (browser.h
    // § teardown; the e10a Windows `!in_dtor_` abort, CE #319 generalised).
    void drain_until(const std::function<bool()>& done);
    // Have the browsers of every live AND retired window finished closing? The shutdown() drain's stop
    // condition — retired browsers are closed in the SAME shared drain, never mid-process.
    [[nodiscard]] bool all_browsers_closed() const;
    [[nodiscard]] WindowEntry* find(WindowId id);
    [[nodiscard]] const WindowEntry* find(WindowId id) const;
    void report_failure(WindowCreateFailure failure);

    std::filesystem::path project_root_;
    EditorStateStore store_;
    std::vector<WindowEntry> windows_;
    std::vector<RetiredSession> retired_;
    WindowFactory factory_;
    WindowCreateFailureSink failure_sink_;
    ChromeMaximizedSink chrome_maximized_sink_;
    std::optional<WindowCreateFailure> last_failure_;
    std::size_t create_failures_ = 0;
    WindowId next_id_ = 0;
    int pumps_ = 0;
    bool shut_down_ = false;
    bool quit_requested_ = false;
};

} // namespace context::editor::shell
