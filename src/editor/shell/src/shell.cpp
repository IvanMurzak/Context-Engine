// The Shell's owner loop and window ownership — see shell.h for the single-threaded pump model and
// the D10 "the Shell is an ordinary authenticated client" rule.

#include "context/editor/shell/shell.h"

#include <cstddef>
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

// ---------------------------------------------------------------------------------- WindowManager

WindowManager::WindowManager(std::filesystem::path project_root)
    : project_root_(std::move(project_root)), store_(project_root_)
{
    store_.load();
}

EditorWindow& WindowManager::add(std::unique_ptr<EditorWindow> window)
{
    const std::size_t index = window->state_index();
    const EditorState& state = store_.state();
    if (index < state.windows.size())
    {
        window->backend().apply_placement(state.windows[index]);
    }
    windows_.push_back(std::move(window));
    return *windows_.back();
}

EditorWindow* WindowManager::window(std::size_t index)
{
    return index < windows_.size() ? windows_[index].get() : nullptr;
}

bool WindowManager::pump_once(std::uint64_t now_us)
{
    ++pumps_;
    for (std::size_t i = 0; i < windows_.size();)
    {
        EditorWindow& window = *windows_[i];
        const bool alive = window.pump_once(now_us);
        if (window.placement_dirty())
        {
            store_.set_placement(window.state_index(), window.last_placement(), now_us);
            window.clear_placement_dirty();
        }
        if (!alive)
        {
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
    for (std::unique_ptr<EditorWindow>& window : windows_)
    {
        window->close();
    }
    windows_.clear();
    // Unconditional, ignoring the debounce: waiting out a quiet period on the way down would just
    // lose the last change the user made.
    (void)store_.flush_now();
}

} // namespace context::editor::shell
