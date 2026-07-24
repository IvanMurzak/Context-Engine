// The Shell's owner loop and window ownership — see shell.h for the single-threaded pump model and
// the D10 "the Shell is an ordinary authenticated client" rule.

#include "context/editor/shell/shell.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>

namespace context::editor::shell
{

// ------------------------------------------------------------------------- the daemon attach (D10)

bool guard_shell_attach(const client::AttachOptions& options, const std::string& discovered_token,
                        std::string& reason)
{
    // `Client::attach` falls back to the token discovery read out of `.editor/instance.json`, so an
    // EMPTY options.token is normal and correct — what is not survivable is neither source having
    // one. Checking here rather than letting the daemon refuse turns "there is no token on this
    // machine" into its own message instead of an `attach.denied` that reads like a wrong password.
    if (options.token.empty() && discovered_token.empty())
    {
        reason = "refusing to attach without the D20 attach token: no token in the attach options "
                 "and none discovered in .editor/instance.json. Token enforcement has been on since "
                 "e02 and the Shell has no unauthenticated path.";
        return false;
    }
    reason.clear();
    return true;
}

client::AttachOptions make_shell_attach_options(std::string token)
{
    client::AttachOptions options;
    options.scope = kShellScope;
    options.capabilities = {"describe"};
    options.token = std::move(token);
    return options;
}

DaemonAttach attach_to_project(const std::filesystem::path& project_root, int timeout_ms)
{
    DaemonAttach result;
    std::string error;
    result.client = client::Client::connect_to_project(project_root, timeout_ms, error);
    if (result.client == nullptr)
    {
        result.error = error.empty() ? "no discoverable daemon for this project" : error;
        return result;
    }

    const client::AttachOptions options = make_shell_attach_options();
    std::string reason;
    if (!guard_shell_attach(options, result.client->instance().token, reason))
    {
        result.error = reason;
        // The connection is dropped rather than kept: an un-attached client is not usable, and
        // holding the socket open would occupy a daemon slot for nothing.
        result.client.reset();
        return result;
    }

    bool rejected_by_daemon = false;
    if (!result.client->attach(options, error, &rejected_by_daemon))
    {
        result.error = error;
        result.error_code = result.client->last_error_code();
        if (result.error_code.empty() && rejected_by_daemon)
        {
            result.error_code = result.client->failure_code("handshake.incompatible_protocol");
        }
        result.client.reset();
        return result;
    }
    result.attached = true;
    return result;
}

// ------------------------------------------------------------------------------------ EditorWindow

EditorWindow::EditorWindow(std::unique_ptr<IWindowBackend> backend,
                           std::unique_ptr<IBrowserHost> browser,
                           const EditorWindowConfig& config)
    : backend_(std::move(backend)), browser_(std::move(browser)), config_(config),
      compositor_(config.compositor)
{
    input_.set_dpi(backend_->dpi());
    last_placement_ = backend_->placement();
}

PresentPath EditorWindow::attach_present(render::IRhi& rhi)
{
    const render::NativeWindowDesc native = backend_->native_window();
    surface_ = rhi.create_surface(native);
    if (surface_ == nullptr)
    {
        diagnostic_ = "no presentable surface for this window; taking the CPU present path";
        attach_cpu_present();
        return compositor_.path();
    }

    // The editor's GPU gate (03 §2). probe_surface creates NO device (R-HEAD-002), so a GPU-less box
    // is answered without paying for a device that would immediately be thrown away.
    const render::AdapterProbe probe = rhi.probe_surface(*surface_);
    if (!probe.has_adapter || !probe.can_present)
    {
        diagnostic_ = "no adapter can present to this window; taking the CPU present path";
        surface_.reset();
        attach_cpu_present();
        return compositor_.path();
    }

    device_ = rhi.create_device();
    if (device_ == nullptr)
    {
        diagnostic_ = "the adapter reported presentable but no device could be created; taking the "
                      "CPU present path";
        surface_.reset();
        attach_cpu_present();
        return compositor_.path();
    }

    if (!compositor_.attach_gpu(*device_, *surface_, backend_->client_size()))
    {
        diagnostic_ = compositor_.diagnostic();
        device_.reset();
        surface_.reset();
        attach_cpu_present();
        return compositor_.path();
    }
    diagnostic_.clear();
    return compositor_.path();
}

void EditorWindow::attach_cpu_present()
{
    const render::NativeWindowDesc native = backend_->native_window();
    render::present::BlitterSelection selection =
        render::present::make_present_blitter(config_.compositor.platform, native.handle);
    if (selection.blitter == nullptr && !selection.diagnostic.empty())
    {
        // Reported, never silent — the platform gap (X11 SHM / CALayer.contents are e12's) is named
        // by the selection itself.
        diagnostic_ = selection.diagnostic;
    }
    compositor_.attach_cpu(std::move(selection.blitter), backend_->client_size());
}

void EditorWindow::sync_browser_size()
{
    // CEF's view rect is DIP, not physical (see IBrowserHost::resize). Converting here, once, is
    // what keeps the browser laying out at the right size on a non-100% monitor.
    const render::Extent2D logical = to_logical(backend_->client_size(), backend_->dpi());
    browser_->resize(logical, backend_->dpi());
    browser_size_synced_ = true;
}

void EditorWindow::handle_event(const ShellEvent& event, std::uint64_t now_us)
{
    switch (event.kind)
    {
    case ShellEventKind::resize:
    {
        compositor_.on_resize(event.size);
        // The resize protocol (03 §4): reconfigure the swapchain (compositor) AND tell the browser
        // (WasResized). Doing only the first leaves the browser painting at the old size and the
        // composite sampling a UV sub-rect that no longer matches the window.
        sync_browser_size();
        break;
    }
    case ShellEventKind::dpi_changed:
    {
        input_.set_dpi(event.dpi);
        // A DPI change with no size change still moves the DIP view rect, so the browser is
        // re-informed even though the physical backbuffer may be unchanged.
        sync_browser_size();
        compositor_.mark_external_damage();
        break;
    }
    case ShellEventKind::moved:
        placement_dirty_ = true;
        break;
    case ShellEventKind::paint_requested:
        compositor_.mark_external_damage();
        break;
    case ShellEventKind::focus_gained:
        browser_->set_focus(true);
        break;
    case ShellEventKind::focus_lost:
        browser_->set_focus(false);
        // The pointer-up that would have released a live drag is going to a different window now.
        input_.cancel_pointer_capture();
        break;
    case ShellEventKind::pointer:
    {
        const PointerDispatch dispatch = input_.route_pointer(event.pointer, now_us);
        switch (dispatch.target)
        {
        case InputTarget::browser:
            browser_->send_pointer(dispatch, event.pointer);
            break;
        case InputTarget::viewport:
        case InputTarget::native:
            // The native path (03 §6.3): camera controls / picking / gizmo gestures. Their panel-model
            // verbs are driven by editor-core over the bridge, which arrives with e11 — until then the
            // arbitration is real and the sample is accounted for, but no native consumer exists yet.
            break;
        case InputTarget::keymap:
        case InputTarget::swallowed:
        default:
            break;
        }
        break;
    }
    case ShellEventKind::key:
    {
        const KeyDispatch dispatch = input_.route_key(event.key, now_us);
        if (dispatch.target == InputTarget::browser)
        {
            browser_->send_key(event.key);
        }
        // A `keymap` target resolves to a command through the keymap that lands with e07.
        break;
    }
    case ShellEventKind::close_requested:
        alive_ = false;
        break;
    case ShellEventKind::none:
    default:
        break;
    }
}

void EditorWindow::poll_placement(std::uint64_t now_us)
{
    if (now_us < last_placement_poll_us_ ||
        (now_us - last_placement_poll_us_) < config_.placement_poll_us)
    {
        return;
    }
    last_placement_poll_us_ = now_us;
    const WindowPlacement current = backend_->placement();
    if (current != last_placement_)
    {
        last_placement_ = current;
        placement_dirty_ = true;
    }
}

bool EditorWindow::pump_once(std::uint64_t now_us)
{
    if (!alive_)
    {
        return false;
    }
    if (!browser_size_synced_)
    {
        sync_browser_size();
    }

    events_.clear();
    const bool window_alive = backend_->pump(events_);
    for (const ShellEvent& event : events_)
    {
        handle_event(event, now_us);
    }
    if (!window_alive)
    {
        alive_ = false;
    }

    // Drive the browser AFTER the OS events: input dispatched this iteration is what a paint should
    // be reacting to, and pumping first would systematically show it one frame late.
    if (!browser_->pump(compositor_))
    {
        // The browser is gone but the window is not: that is the CEF-renderer-crash path (03 §7),
        // which recovers by respawning. Recorded and left to the caller rather than closing the
        // window, because closing it would lose the layout the recovery is supposed to restore.
        diagnostic_ = "the browser host ended; the window is still alive";
    }

    poll_placement(now_us);

    if (alive_)
    {
        // Damage-driven: render_frame() is a no-op when nothing changed (see compositor.h).
        (void)compositor_.render_frame();
    }
    return alive_;
}

void EditorWindow::close()
{
    if (browser_ != nullptr)
    {
        browser_->close();
    }
    compositor_.detach();
    if (backend_ != nullptr)
    {
        backend_->close();
    }
    alive_ = false;
}

void EditorWindow::begin_close()
{
    // Phase 1: unbind the browser's sink and ask CEF to close it, but do NOT pump — the manager
    // drives ONE shared drain for every closing window (browser.h § teardown). The compositor and
    // backend stay attached until finish_close(): the browser's sink is already unbound here, so no
    // frame reaches the compositor during the drain that follows.
    if (browser_ != nullptr)
    {
        browser_->request_close();
    }
}

bool EditorWindow::browser_closed() const
{
    return browser_ == nullptr || browser_->is_closed();
}

void EditorWindow::detach_browser()
{
    // Mid-process retire: unbind the browser's frame sink so it stops painting into a compositor that
    // is about to go away, but do NOT close the browser — its CEF teardown is deferred to the shared
    // all-closing shutdown() drain (browser.h § IBrowserHost::detach; the e10a `!in_dtor_` fix).
    if (browser_ != nullptr)
    {
        browser_->detach();
    }
}

void EditorWindow::finish_close()
{
    // Phase 3: the non-CEF teardown, run after the shared drain confirmed the browser closed. The
    // browser is deliberately NOT re-closed here (request_close() already did, and the drain
    // completed it); resetting this window destroys the host, whose destructor close() is then a
    // no-op on the already-closed browser.
    compositor_.detach();
    if (backend_ != nullptr)
    {
        backend_->close();
    }
    alive_ = false;
}

// ---------------------------------------------------------------------------------- WindowManager

WindowManager::WindowManager(std::filesystem::path project_root)
    : project_root_(std::move(project_root)), store_(project_root_)
{
    store_.load();
}

// The graveyard is emptied HERE and nowhere else (shell.h § the class note / window_registry.h
// § LIFETIME RULE). By the time this runs the app has already called `shell::cef::shutdown()`, so
// CEF has finished dispatching to every client that held one of these routers.
WindowManager::~WindowManager() = default;

EditorWindow& WindowManager::add(std::unique_ptr<EditorWindow> window)
{
    // add_session always push_backs, so the adopted window IS the last entry — no lookup, and no
    // null case to guard.
    add_session(std::move(window), WindowSessionParts{});
    return *windows_.back().window;
}

WindowId WindowManager::add_session(std::unique_ptr<EditorWindow> window,
                                    WindowSessionParts&& session)
{
    const std::size_t index = window->state_index();
    const EditorState& state = store_.state();
    if (index < state.windows.size())
    {
        window->backend().apply_placement(state.windows[index]);
    }

    WindowEntry entry;
    entry.id = next_id_++;
    entry.surfaces = std::move(session.surfaces);
    entry.bridge = std::move(session.bridge);
    entry.daemon_client = std::move(session.daemon_client);
    entry.window = std::move(window);
    windows_.push_back(std::move(entry));
    return windows_.back().id;
}

void WindowManager::bind_window_factory(WindowFactory factory)
{
    factory_ = std::move(factory);
}

void WindowManager::on_window_create_failed(WindowCreateFailureSink sink)
{
    failure_sink_ = std::move(sink);
}

void WindowManager::report_failure(WindowCreateFailure failure)
{
    ++create_failures_;
    // LOUD by default (03 §7): the report reaches stderr even when nothing bound a sink, because a
    // window that silently did not open is indistinguishable from one that opened offscreen.
    std::fprintf(stderr, "[shell] %s\n", describe(failure).c_str());
    if (failure_sink_)
    {
        failure_sink_(failure);
    }
    last_failure_ = std::move(failure);
}

WindowCreateResult WindowManager::create_window(const WindowSpec& spec, WindowId source)
{
    WindowCreateResult result;
    result.id = kInvalidWindowId;

    const auto fail = [&](WindowCreateOutcome outcome, std::string error)
    {
        result.outcome = outcome;
        result.error = std::move(error);
        WindowCreateFailure failure;
        failure.outcome = outcome;
        failure.source = source;
        failure.title = spec.title;
        failure.error = result.error;
        report_failure(std::move(failure));
        return result;
    };

    if (windows_.size() >= kMaxEditorWindows)
    {
        return fail(WindowCreateOutcome::limit_reached,
                    "the editor already has the maximum of " + std::to_string(kMaxEditorWindows) +
                        " windows open");
    }
    if (factory_ == nullptr)
    {
        return fail(WindowCreateOutcome::no_factory,
                    "no window factory is bound — this build cannot create a window");
    }

    WindowSessionParts parts;
    std::string error;
    if (!factory_(spec, parts, error))
    {
        return fail(WindowCreateOutcome::factory_failed,
                    error.empty() ? std::string("the window factory reported a failure with no "
                                                "reason")
                                  : error);
    }
    if (!validate_window_parts(parts, error))
    {
        // A factory that says "yes" and hands back nothing usable is a DIFFERENT defect from one
        // that says "no", and it is reported as such rather than crashing on the null browser.
        return fail(WindowCreateOutcome::incomplete_parts, error);
    }

    EditorWindowConfig config;
    config.state_index = spec.state_index;
    auto window = std::make_unique<EditorWindow>(std::move(parts.backend), std::move(parts.browser),
                                                 config);
    if (spec.placement.has_value())
    {
        window->backend().apply_placement(*spec.placement);
    }
    // The window has NO present path yet: attaching one needs an RHI the registry does not own, so
    // the caller does it (exactly as the app does for window 0). A window with no present path
    // composites nothing, which is why the factory's diagnostic is surfaced by the caller too.
    result.id = add_session(std::move(window), std::move(parts));
    result.outcome = WindowCreateOutcome::created;
    result.error.clear();
    return result;
}

void WindowManager::retire(WindowEntry& entry)
{
    if (entry.window != nullptr)
    {
        // Stop the browser painting (unbind its sink) and release the OS/GPU resources (compositor +
        // backend) — but KEEP THE BROWSER HOST ALIVE. Closing + draining it here, mid-process, drives
        // `CefDoMessageLoopWork()` through this browser's CEF teardown while sibling browsers are live
        // in the same process-wide loop, which on Windows re-enters a libcef ref-counted object's own
        // destructor (the `!in_dtor_` abort; CE #319 generalised). So its CEF teardown is deferred: the
        // browser is closed only in `shutdown()`'s ONE all-closing drain, and the host is destroyed only
        // by ~WindowManager after `shell::cef::shutdown()` (window_registry.h § LIFETIME RULE).
        entry.window->detach_browser();
        entry.window->finish_close();
    }
    // Move the WHOLE session — the window (with its still-open browser host) plus the bridge, the
    // daemon client and the captured surfaces — into the graveyard, freed only by ~WindowManager.
    // Member order there is destruction order reversed (browser first, surfaces last).
    RetiredSession retired;
    retired.surfaces = std::move(entry.surfaces);
    retired.bridge = std::move(entry.bridge);
    retired.daemon_client = std::move(entry.daemon_client);
    retired.window = std::move(entry.window);
    if (retired.window != nullptr || retired.bridge != nullptr ||
        retired.daemon_client != nullptr || !retired.surfaces.empty())
    {
        retired_.push_back(std::move(retired));
    }
}

void WindowManager::drain_until(const std::function<bool()>& done)
{
    // `CefDoMessageLoopWork()` is PROCESS-WIDE, so pumping through ANY one browser drains every
    // CLOSING browser at once. This is the ONE shared drain, run ONLY from `shutdown()` after EVERY
    // browser — live and retired — has been asked to close: with nothing left live, the pump does
    // only teardown work, so no browser's final destruction is re-entered by a sibling's live pump
    // (the `!in_dtor_` abort; CE #319 generalised). The budget mirrors the old per-window 200-slice
    // cap, scaled by the total browser count (live + retired) so a many-window teardown keeps the
    // same per-browser headroom; a host with no message loop reports closed immediately, so this
    // runs zero slices for the unit fakes.
    const int budget =
        200 * (static_cast<int>(windows_.size() + retired_.size()) + 1);
    for (int i = 0; i < budget && !done(); ++i)
    {
        IBrowserHost* pump = nullptr;
        for (WindowEntry& entry : windows_)
        {
            if (entry.window != nullptr)
            {
                pump = entry.window->browser_or_null();
                if (pump != nullptr)
                {
                    break;
                }
            }
        }
        // If every live window is gone (e.g. all destroyed mid-process), a retired browser still
        // drives the same process-wide loop — any host will do, the work drained is process-wide.
        if (pump == nullptr)
        {
            for (RetiredSession& retired : retired_)
            {
                if (retired.window != nullptr)
                {
                    pump = retired.window->browser_or_null();
                    if (pump != nullptr)
                    {
                        break;
                    }
                }
            }
        }
        if (pump == nullptr)
        {
            break;
        }
        pump->pump_teardown();
    }
}

bool WindowManager::all_browsers_closed() const
{
    for (const WindowEntry& entry : windows_)
    {
        if (entry.window != nullptr && !entry.window->browser_closed())
        {
            return false;
        }
    }
    // Retired browsers are closed in the SAME shared drain (never mid-process), so the drain's stop
    // condition must wait on them too.
    for (const RetiredSession& retired : retired_)
    {
        if (retired.window != nullptr && !retired.window->browser_closed())
        {
            return false;
        }
    }
    return true;
}

WindowDestroyResult WindowManager::destroy_window(WindowId id)
{
    WindowDestroyResult result;
    if (is_primary(id))
    {
        result.outcome = WindowDestroyOutcome::primary_refused;
        result.error = "window 0 is primary (it hosts the app menu + welcome screen); it closes "
                       "with the app, not on its own";
        return result;
    }
    for (std::size_t i = 0; i < windows_.size(); ++i)
    {
        if (windows_[i].id != id)
        {
            continue;
        }
        // Detach + retire the WHOLE session (browser host included) into the graveyard. The browser is
        // NOT closed here — closing it mid-process, while sibling browsers are live, is the `!in_dtor_`
        // re-entrancy; its CEF teardown is deferred to the shared `shutdown()` drain (see retire()).
        retire(windows_[i]);
        windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(i));
        result.outcome = WindowDestroyOutcome::destroyed;
        result.error.clear();
        return result;
    }
    result.outcome = WindowDestroyOutcome::unknown_window;
    result.error = "no live window carries id " + std::to_string(static_cast<unsigned long long>(id));
    return result;
}

WindowManager::WindowEntry* WindowManager::find(WindowId id)
{
    for (WindowEntry& entry : windows_)
    {
        if (entry.id == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

const WindowManager::WindowEntry* WindowManager::find(WindowId id) const
{
    for (const WindowEntry& entry : windows_)
    {
        if (entry.id == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

EditorWindow* WindowManager::window(WindowId id)
{
    WindowEntry* entry = find(id);
    return entry != nullptr ? entry->window.get() : nullptr;
}

std::vector<WindowId> WindowManager::window_ids() const
{
    std::vector<WindowId> ids;
    ids.reserve(windows_.size());
    for (const WindowEntry& entry : windows_)
    {
        ids.push_back(entry.id);
    }
    return ids;
}

std::uint64_t WindowManager::window_origin(WindowId id) const
{
    const WindowEntry* entry = find(id);
    if (entry == nullptr)
    {
        return 0;
    }
    // A session that owns its connection answers from it — the id the DAEMON minted, never a value
    // the Shell chose for itself.
    if (entry->daemon_client != nullptr)
    {
        return entry->daemon_client->client_id();
    }
    return entry->origin;
}

void WindowManager::set_window_origin(WindowId id, std::uint64_t origin)
{
    if (WindowEntry* entry = find(id))
    {
        entry->origin = origin;
    }
}

std::size_t WindowManager::distinct_origins() const
{
    std::vector<std::uint64_t> seen;
    for (const WindowEntry& entry : windows_)
    {
        const std::uint64_t origin = window_origin(entry.id);
        if (origin == 0)
        {
            // 0 is "no identity" (e08a: it also means "not attached"), so it is never counted as
            // one — two unattached windows are not two origins.
            continue;
        }
        bool known = false;
        for (const std::uint64_t other : seen)
        {
            if (other == origin)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            seen.push_back(origin);
        }
    }
    return seen.size();
}

bool WindowManager::pump_once(std::uint64_t now_us)
{
    ++pumps_;
    for (std::size_t i = 0; i < windows_.size();)
    {
        EditorWindow& window = *windows_[i].window;
        const bool alive = window.pump_once(now_us);
        if (window.placement_dirty())
        {
            store_.set_placement(window.state_index(), window.last_placement(), now_us);
            window.clear_placement_dirty();
        }
        if (!alive)
        {
            // A window that died on its own is retired exactly like an explicitly destroyed one: its
            // session (browser host included) must not be freed while CEF may still reach it. Its
            // browser's CEF teardown is deferred to the shared shutdown() drain, never driven here
            // mid-process while sibling browsers are live (retire(); the `!in_dtor_` fix).
            retire(windows_[i]);
            windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    // Debounced from the FIRST dirtying change, not from the last: a window drag marks the store
    // dirty on every move, and this writes once `debounce_us` has elapsed since the move that
    // started the dirty run — then re-arms. A long drag therefore checkpoints every window rather
    // than writing only after the user stops. Deliberate: a crash mid-drag keeps recent placement,
    // and shutdown() flushes unconditionally anyway (see editor_state.h).
    (void)store_.flush_if_due(now_us);
    return !windows_.empty();
}

void WindowManager::shutdown()
{
    if (shut_down_)
    {
        return;
    }
    shut_down_ = true;

    // Teardown in THREE phases, serialised so EVERY browser closes together instead of one at a time.
    // A per-window close + drain runs the process-wide `CefDoMessageLoopWork()` through one browser's
    // teardown while others are still live, and on Windows that reaches a libcef ref-counted object's
    // final Release inside its own destructor (the `!in_dtor_` abort; CE #319 generalised to N
    // windows). Asking every browser to close FIRST, then draining ONCE, then releasing, is CEF's own
    // multi-browser shutdown shape and removes that interleaving. This is ALSO where the browsers of
    // sessions RETIRED mid-process (destroy_window / a self-death) are finally closed — retire() left
    // them open precisely so they could be closed here, with nothing live, rather than mid-process.

    // Phase 1: ask every browser to close (unbind its sink + CloseBrowser), pumping NOTHING — the live
    // windows AND the retired-but-still-open sessions.
    for (WindowEntry& entry : windows_)
    {
        if (entry.window != nullptr)
        {
            entry.window->begin_close();
        }
    }
    for (RetiredSession& retired : retired_)
    {
        if (retired.window != nullptr)
        {
            retired.window->begin_close();
        }
    }
    // Phase 2: ONE shared drain that completes every browser's pending OnBeforeClose (live + retired).
    drain_until([this] { return all_browsers_closed(); });
    // Phase 3: finish + RETIRE each still-live window to the graveyard — still BEFORE
    // shell::cef::shutdown() in the app, so no router (or browser host) is freed while CEF may still
    // reach it (CE #319). The already-retired sessions stay put; their browsers were closed in phase 2.
    for (WindowEntry& entry : windows_)
    {
        retire(entry);
    }
    windows_.clear();
    // Unconditional, ignoring the debounce: waiting out a quiet period on the way down would just
    // lose the last change the user made.
    (void)store_.flush_now();
}

} // namespace context::editor::shell
