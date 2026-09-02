// The Shell's owner loop and window ownership — see shell.h for the single-threaded pump model and
// the D10 "the Shell is an ordinary authenticated client" rule.

#include "context/editor/shell/shell.h"

#include "context/editor/shell/cocoa_chrome.h"

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
    // c1 (editor-window-chrome, target design 02 §4): a macOS backend consults THIS window's live
    // input arbiter at NSEvent time — its published region map AND its capture state — so a press
    // on the `caption` rect becomes the OS's window drag exactly when route_pointer would have
    // credited the caption, and never while a live capture owns the press. Wired at construction,
    // unconditionally, so EVERY composition (the app, the windowed smoke, a test) gets production
    // wiring; a no-op for every non-Cocoa backend. Lifetime is sound by scope: the arbiter is a
    // member of this same object and the backend reads the pointer only inside pump(), which never
    // runs during destruction.
    cocoa_bind_caption_arbiter(*backend_, &input_);
    // b1 (D11): the browser reports `StartDragging` / `UpdateDragCursor` HERE, so an HTML5 drag is
    // driven instead of refused. Bound unconditionally at construction, exactly like the caption
    // arbiter above and for the same reason — every composition (the app, the smokes, a test) gets
    // production wiring, and a window whose drag observer was bound only on some paths is a window
    // where the drag is dead on the others.
    browser_->set_drag_observer(&drag_observer_);
}

// --- b1: the browser's drag observer -------------------------------------------------------------

bool EditorWindow::DragObserver::on_start_dragging(DragOperationMask allowed, PointI start_view_dip)
{
    // The window's answer IS `StartDragging`'s return value. `begin()` refuses only a SECOND
    // overlapping drag (osr_drag.h), so the ordinary case is true — which is the whole of the fix:
    // the unimplemented default returned false, and the pinned header defines that as "abort the
    // drag operation".
    return owner_->drag_.begin(allowed, start_view_dip);
}

void EditorWindow::DragObserver::on_update_drag_cursor(DragOperation operation)
{
    // ⚠ ONLY WHILE A DRAG IS LIVE, and that gate is load-bearing rather than defensive. CEF keeps
    // dispatching this callback for a beat AFTER the drag is over — the `DragTargetDrop` /
    // `DragSourceEndedAt` the Shell just injected are answered on a later `CefDoMessageLoopWork`,
    // typically with `DRAG_OPERATION_NONE`, which arrives after `apply_drag` has already restored
    // the ordinary cursor. Without this early return that late `none` would be read as "the thing
    // under the pointer refuses the drop" (the disambiguation below) and leave the editor showing
    // the NO-DROP cursor for good: nothing resets it until the NEXT drag ends. Same story on the
    // `escaped` / `focus_lost` cancels, which end the session while the renderer is still talking.
    // The session-state half was already gated — `set_operation` no-ops when inactive — so this
    // only brings the OS half into line with it.
    if (!owner_->drag_.active())
    {
        return;
    }
    owner_->drag_.set_operation(operation);
    // Straight through to the OS: this callback is the ONLY source of drag feedback, and CEF sends
    // it only when the answer CHANGES, so pushing it down here is one OS call per change rather
    // than one per pointer sample. THROUGH `drag_cursor_for`, which is where CEF's overloaded
    // `DRAG_OPERATION_NONE` is disambiguated — during a drag it means "this will not take the
    // drop", never "there is no drag" (osr_drag.h § the feedback cursor).
    owner_->push_drag_cursor(drag_cursor_for(operation));
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
    // e3: the viewport producer draws into targets created on THIS device. Adopted here, at the one
    // place a device comes into existence, so a viewport can never be rendered against a device the
    // window has since replaced.
    viewports_.attach_device(*device_);
    diagnostic_.clear();
    return compositor_.path();
}

void EditorWindow::attach_cpu_present()
{
    const render::NativeWindowDesc native = backend_->native_window();
    // The WHOLE native-window descriptor, not a platform tag plus a handle (e12a; docs/present-path.md
    // § the e03 follow-up): a 2D present primitive is window-system-granular, and X11 needs the
    // Display* alongside the Window.
    // e3: no device on this path, so the viewports degrade HONESTLY rather than silently — their
    // layers still publish (the rects and the routing are unaffected) but carry no content, and the
    // panel feed reports `viewport.adapter_absent` (R-HEAD-002). Detached BEFORE the blitter is
    // selected, so an early return below cannot leave a binding pointing at a dead device.
    viewports_.detach_device();
    render::present::BlitterSelection selection = render::present::make_present_blitter(native);
    if (selection.blitter == nullptr && !selection.diagnostic.empty())
    {
        // Reported, never silent — and since e12b there is no platform GAP left to name: every v1
        // window system has a blitter, so what the selection reports is a missing X11 build
        // dependency, a window carrying no presentable surface, or a non-v1 window system.
        diagnostic_ = selection.diagnostic;
    }
    compositor_.attach_cpu(std::move(selection.blitter), backend_->client_size());
}

void EditorWindow::sync_browser_size()
{
    // The window's geometry, read ONCE and pushed to both consumers. Reading the pair here rather
    // than in each arm of handle_event is what keeps the browser and the compositor from ever being
    // told a size and a scale sampled at two different moments.
    const render::Extent2D physical = backend_->client_size();
    const DpiScale dpi = backend_->dpi();
    // CEF's view rect is DIP, not physical (see IBrowserHost::resize). Converting here, once, is
    // what keeps the browser laying out at the right size on a non-100% monitor.
    const render::Extent2D logical = to_logical(physical, dpi);
    browser_->resize(logical, dpi);
    // a2: the compositor takes the PHYSICAL size and the SAME scale. It needs the scale because the
    // one geometry CEF reports in DIP is the popup rect (OnPopupSize), which the compositor draws
    // onto a physical surface — see WindowCompositor::popup_dest_rect. Pushing it HERE, beside the
    // browser's, is what makes the first pump's sync (`browser_size_synced_`, pump_once) seed the
    // scale as well: a window opened on a 150 % monitor that is never resized still composites its
    // dropdowns correctly. The `resize` arm below therefore no longer calls on_resize itself — the
    // backend has already applied the event to itself by then, so `client_size()` IS `event.size`
    // (window.cpp / x11_window.cpp both state that contract).
    compositor_.on_resize(physical, dpi);
    // A resize can MOVE the client origin as well (a maximize does both), and a DPI change moves it
    // in DIP terms even when the physical rect is unchanged — so the origin rides along here rather
    // than waiting for the next move event or placement poll. MARKED, not pushed: a modal resize
    // drag delivers one `resize` per frame into a single drain, and the push is worth doing once.
    origin_dirty_ = true;
    browser_size_synced_ = true;
}

void EditorWindow::sync_browser_origin()
{
    browser_->set_client_origin(backend_->client_origin());
}

namespace
{

// VK_ESCAPE, the cancel gesture of every drag protocol on every one of the three platforms. Named
// rather than spelled `0x1B` at the comparison, matching the decoder tables in window.cpp that
// PRODUCE it (their own `kVkEscape`); this is the consuming side of the same constant.
constexpr std::int32_t kVkEscape = 0x1B;

// Is a PHYSICAL client-pixel sample inside the browser's view?
//
// PHYSICAL, not DIP, and against the backend's own `client_size()`: that is the unit every
// `ShellEvent` position is in (input.h), so no rounding enters the membership test. The DIP point
// the injection carries is converted separately by the arbiter — a test done in DIP would disagree
// with the arbiter's rounding at the one-pixel border, and "did the pointer leave the view" is
// exactly the question a one-pixel disagreement makes flap.
[[nodiscard]] bool inside_client(PointI position, render::Extent2D client)
{
    return position.x >= 0 && position.y >= 0 &&
           position.x < static_cast<std::int32_t>(client.width) &&
           position.y < static_cast<std::int32_t>(client.height);
}

} // namespace

void EditorWindow::apply_drag(const OsrDragInjections& injections)
{
    for (const OsrDragInjection& injection : injections)
    {
        browser_->inject_drag(injection);
    }
    if (!drag_.active() && !injections.empty())
    {
        // The drag ENDED in this batch: put the ordinary cursor back. CEF sends no final
        // `UpdateDragCursor(none)` of its own, so a feedback cursor left standing would outlive the
        // gesture — the cosmetic cousin of the leaked cursor capture `cross_window_drag.h` guards
        // against, and equally invisible to anything but a human.
        push_drag_cursor(DragCursor::none);
    }
}

// ONE PUSH PER CHANGE — see shell.h. Deliberately NOT gated on `drag_.active()`: the end-of-drag
// reset runs precisely when the session has already gone inactive.
void EditorWindow::push_drag_cursor(DragCursor cursor)
{
    if (cursor == last_drag_cursor_)
    {
        return;
    }
    last_drag_cursor_ = cursor;
    backend_->set_drag_cursor(cursor);
}

void EditorWindow::drive_drag(const PointerDispatch& dispatch, const PointerEvent& event)
{
    const bool inside = event.action != PointerAction::leave &&
                        inside_client(event.position, backend_->client_size());
    switch (event.action)
    {
    case PointerAction::up:
        // A DRAG ENDS ON ITS OWN BUTTON, not on any button: a right-click released mid-drag is not
        // a drop, and treating it as one would commit a rehome the user never asked for. CEF starts
        // the drag from a left-button gesture, so that is the one that ends it.
        if (event.button == MouseButton::left)
        {
            apply_drag(drag_.release(dispatch.logical_position, event.modifiers, inside));
            break;
        }
        apply_drag(drag_.move(dispatch.logical_position, event.modifiers, inside));
        break;
    case PointerAction::wheel:
        // A wheel sample carries no position change worth reporting and has no meaning inside the
        // drag protocol — CEF exposes no `DragTarget*` wheel member at all. Swallowed rather than
        // forwarded: `SendMouseWheelEvent` during a drag is precisely the stray ordinary mouse
        // event the protocol replaces.
        break;
    case PointerAction::move:
    case PointerAction::down:
    case PointerAction::leave:
    default:
        apply_drag(drag_.move(dispatch.logical_position, event.modifiers, inside));
        break;
    }
}

void EditorWindow::end_drag(OsrDragEndReason reason, bool inject)
{
    if (!drag_.active())
    {
        return;
    }
    const OsrDragInjections injections = drag_.cancel(reason);
    if (inject)
    {
        apply_drag(injections);
        return;
    }
    // TEARDOWN: the browser is closing (or already gone), so the injections have no reader. The
    // session is still ENDED rather than left active — `drags_begun()`/`drags_ended()` is the pair
    // that says a drag never dangled, and a window torn down mid-drag must not be the one case
    // where they disagree. The cursor is restored through the backend, which outlives the browser.
    push_drag_cursor(DragCursor::none);
}

void EditorWindow::handle_event(const ShellEvent& event, std::uint64_t now_us)
{
    switch (event.kind)
    {
    case ShellEventKind::resize:
    {
        // The resize protocol (03 §4): reconfigure the swapchain (compositor) AND tell the browser
        // (WasResized). Doing only the first leaves the browser painting at the old size and the
        // composite sampling a UV sub-rect that no longer matches the window. Both halves live in
        // sync_browser_size() since a2, so that the size and the scale are read together.
        sync_browser_size();
        break;
    }
    case ShellEventKind::dpi_changed:
    {
        input_.set_dpi(event.dpi);
        // A DPI change with no size change still moves the DIP view rect, so the browser is
        // re-informed even though the physical backbuffer may be unchanged — and so is the
        // compositor, whose popup conversion is scale-dependent (a2).
        sync_browser_size();
        compositor_.mark_external_damage();
        break;
    }
    case ShellEventKind::moved:
        placement_dirty_ = true;
        // a1: the window moved, so every view->screen mapping the browser answers is now stale.
        // Marked on the EVENT rather than only on the 250 ms placement poll: a context menu opened
        // straight after a drag would otherwise be placed where the window used to be. The push
        // itself is coalesced to once per pump (`origin_dirty_`), which costs no freshness — the
        // single read happens LATER than the last of the reads it replaces, and still before the
        // browser pump that is the only reader.
        origin_dirty_ = true;
        break;
    case ShellEventKind::paint_requested:
        compositor_.mark_external_damage();
        break;
    case ShellEventKind::focus_gained:
        focused_ = true; // a1: `chrome.state.focused` reads this — the OS event is the truth source
        browser_->set_focus(true);
        break;
    case ShellEventKind::focus_lost:
        focused_ = false;
        browser_->set_focus(false);
        // The pointer-up that would have released a live drag is going to a different window now.
        input_.cancel_pointer_capture();
        // b1: and for the SAME reason, the OSR drag can never be completed — its release will be
        // delivered somewhere else entirely. Ended here rather than left hanging, because a drag
        // the renderer believes is still running keeps its drop targets armed forever. The
        // injections DO go out: the browser is alive and it is the thing that must be told.
        end_drag(OsrDragEndReason::focus_lost, /*inject*/ true);
        break;
    case ShellEventKind::pointer:
    {
        const PointerDispatch dispatch = input_.route_pointer(event.pointer, now_us);
        if (drag_.active())
        {
            // THE DRAG REPLACES THE MOUSE STREAM, it does not ride beside it. While a drag runs
            // CEF's own contract is that the view is fed through `DragTarget*`
            // (cef_browser.h:897-927); an ordinary `SendMouseMoveEvent` delivered in the same
            // window is a second, contradictory description of where the pointer is, and it is what
            // makes a drop land on the element the drag passed OVER rather than the one it ended
            // on. Routed unconditionally rather than only for `InputTarget::browser`: a drag can
            // only have begun from browser content in the first place, and a mid-drag region claim
            // (a caption strip the cursor happens to cross) must not silently split the gesture
            // between two consumers.
            drive_drag(dispatch, event.pointer);
            break;
        }
        switch (dispatch.target)
        {
        case InputTarget::browser:
            browser_->send_pointer(dispatch, event.pointer);
            break;
        case InputTarget::viewport:
        case InputTarget::native:
            // The native path (03 §6.3): camera controls / picking / gizmo gestures, and the
            // CAPTION drag surface (editor-window-chrome b1/c1, 02 §6): both OS consumers sit
            // UPSTREAM of this arm. On Windows the NC hit-test consumes caption points BEFORE
            // client routing (they arrive as NC messages the pump never forwards); on macOS c1's
            // pump consumes a caption PRESS at NSEvent time (cocoa_window.mm's caption consult —
            // only there does performWindowDragWithEvent: still have the event), so what reaches
            // here is the press's aftermath (its release, hovers over the strip) — and on a backend
            // with no native consumer at all (the headless smokes) the caption samples themselves.
            // Dropping them here IS the suppression: a caption press must never half-reach the
            // browser (ROADMAP risk 3). The caption CONTROLS are deliberately NOT this arm's —
            // they are web-drawn browser content and route InputTarget::browser (input.cpp).
            //
            // ⚠ THE VIEWPORT ARM IS STILL EMPTY, and that is a scope statement rather than an
            // oversight. editor-UX e3 landed the viewport's PRODUCER and its camera TRANSPORT (the
            // opaque `editor.camera-set` payload, hydrated from `editor.cameras-get`), and the one
            // thing that MOVES a camera today is the panel's keyboard-reachable `viewport.frame-scene`
            // command — which arrives over `panel.invoke`, not here. Orbit/pan/zoom gesture math and
            // picking are e4 and its successors; a half-written gesture here would be a second,
            // untested camera writer beside the one the feed already owns.
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
        // b1: ESCAPE CANCELS A LIVE DRAG, and it is consumed rather than forwarded. The renderer
        // has no drag loop of its own to escape from — the Shell owns the loop (osr_drag.h) — so a
        // key event delivered here instead would reach the document as an ordinary Escape while the
        // drag went on running. Ahead of `route_key` on purpose: a cancel must not depend on where
        // editor-core says focus is (`FocusClass`), because a drag in flight is exactly when a
        // stuck gesture is least recoverable.
        if (drag_.active() && event.key.windows_key_code == kVkEscape &&
            (event.key.action == KeyAction::raw_key_down || event.key.action == KeyAction::key_down))
        {
            end_drag(OsrDragEndReason::escaped, /*inject*/ true);
            break;
        }
        const KeyDispatch dispatch = input_.route_key(event.key, now_us);
        if (dispatch.target == InputTarget::browser)
        {
            browser_->send_key(event.key);
        }
        // A `keymap` target resolves to a command through the keymap that lands with e07.
        break;
    }
    case ShellEventKind::close_requested:
        // b1: the window is going away, but the browser is still live and still pumping this
        // iteration — so the drag is ended THROUGH the protocol rather than abandoned.
        end_drag(OsrDragEndReason::window_closed, /*inject*/ true);
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
        // a1: the backstop for the push above. A maximize/restore, a WM-driven move, or a backend
        // that reports no `moved` event at all still lands here within one poll interval — and the
        // maximized case is exactly the one `placement()` alone cannot answer, which is why the
        // origin is re-read from the backend rather than derived from `current`.
        //
        // Pushed INLINE rather than through `origin_dirty_`: this runs AFTER the browser pump, so a
        // deferred flag would not be consumed until the next iteration, and there is nothing to
        // coalesce here anyway — the poll interval plus this `!=` gate already admit at most one
        // push per pump.
        sync_browser_origin();
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

    // b1: push a REPUBLISHED region map down to the OS backend BEFORE draining its queue, so the
    // WM_NCHITTEST answered during this pump (and during any modal drag loop entered from it) sees
    // the newest chrome rects. Generation-gated — one integer compare per pump, the exact cheap
    // change-detection the RegionMap's generation counter exists for (input.h) — and wholesale,
    // mirroring RegionMap::publish. The publish itself lands during a LATER stage of the previous
    // pump (the bridge call runs inside browser_->pump), so worst-case staleness is one iteration.
    if (input_.regions().generation() != chrome_regions_pushed_generation_)
    {
        chrome_regions_pushed_generation_ = input_.regions().generation();
        backend_->set_chrome_regions(input_.regions().regions());
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

    // a1: the ONE origin push for this iteration, collapsing however many `moved`/`resize`/
    // `dpi_changed` events the drain above carried (a whole caption drag arrives as one batch —
    // see `origin_dirty_`). Here rather than inside the arms because `client_origin()` reads the
    // LIVE position, so the last read wins anyway; and before the browser pump below, which is the
    // only thing that reads the value back out.
    if (origin_dirty_)
    {
        origin_dirty_ = false;
        sync_browser_origin();
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

    // e3: rebuild the viewport layer stack from the window's region map — AFTER the browser drain,
    // because editor-core's `editor.regions.publish` lands INSIDE `browser_->pump()` (the bridge
    // call), so publishing before it would composite this iteration against last iteration's layout.
    // (The chrome push above cannot move: an NC hit-test answered during the drain needs the rects
    // BEFORE it, and it pays the one-iteration staleness that buys.)
    //
    // GENERATION-GATED, one integer compare per pump — the change detection RegionMap's counter
    // exists for, and its first consumer. A viewport whose CONTENT moved without its rect moving is
    // not covered by this gate and never will be: that is `mark_viewport_content()`'s job, which the
    // producer marks itself, and which a live scene feed will drive when one exists.
    if (alive_ && input_.regions().generation() != viewport_regions_generation_)
    {
        viewport_regions_generation_ = input_.regions().generation();
        (void)viewports_.publish(input_.regions().regions(), viewport_scene_, compositor_);
    }

    if (alive_)
    {
        // Damage-driven: render_frame() is a no-op when nothing changed (see compositor.h).
        (void)compositor_.render_frame();
    }
    return alive_;
}

void EditorWindow::close()
{
    // b1: end a live drag BEFORE the browser goes, without injecting — the host is one line from
    // its own close, so a `DragTarget*` call here would be a message to something that is already
    // tearing down. Ending it anyway keeps `drags_begun()`/`drags_ended()` balanced on every path,
    // which is what makes "no drag ever dangled" an assertable property rather than a hope.
    end_drag(OsrDragEndReason::window_closed, /*inject*/ false);
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
    end_drag(OsrDragEndReason::window_closed, /*inject*/ false); // b1 — see close() for why not
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
    end_drag(OsrDragEndReason::window_closed, /*inject*/ false); // b1 — see close() for why not
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
    // Re-sync the window's placement cache with the backend: the restore above mutated the backend
    // AFTER the EditorWindow constructor cached its placement, and pump_once's chrome-fact compare
    // reads that cache — seeding the baseline from one source while comparing against the other
    // would fire a spurious flip on every pump until the first poll refreshed the cache. (It also
    // keeps the first poll from re-persisting the placement that was just restored.)
    entry.window->sync_placement_baseline();
    // Seed the chrome-fact baseline from the SAME source pump_once compares against — the freshly
    // synced cache — so a window restored maximized does not fire a spurious flip at boot; the sink
    // reports CHANGES; `chrome.state` reports the initial state.
    entry.last_maximized = entry.window->last_placement().maximized;
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

void WindowManager::on_chrome_maximized(ChromeMaximizedSink sink)
{
    chrome_maximized_sink_ = std::move(sink);
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

WindowDestroyResult WindowManager::close_window(WindowId id)
{
    // An unknown id is answered FIRST, before the primary test, so a stale id can never quit the
    // app: `find` is the same lookup destroy_window uses, and window 0 always resolves while it is
    // live. (After the last window is gone the manager has nothing to close and says so.)
    if (find(id) == nullptr)
    {
        WindowDestroyResult unknown;
        unknown.outcome = WindowDestroyOutcome::unknown_window;
        unknown.error =
            "no live window carries id " + std::to_string(static_cast<unsigned long long>(id));
        return unknown;
    }
    if (!is_primary(id))
    {
        return destroy_window(id);
    }
    request_quit();
    WindowDestroyResult quit;
    quit.outcome = WindowDestroyOutcome::app_quit;
    quit.error.clear();
    return quit;
}

void WindowManager::request_quit()
{
    quit_requested_ = true;
    // ASK, never destroy: see the header. Closing a window here would free a session out from under
    // the pump that is very likely running above this call (a `window.close` arrives INSIDE a
    // browser pump, which runs inside `pump_once`'s loop over `windows_`) — the same re-entrancy
    // `destroy_window` avoids by retiring rather than tearing down.
    for (WindowEntry& entry : windows_)
    {
        if (entry.window != nullptr)
        {
            entry.window->backend().close();
        }
    }
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
        // a1: report a maximized FLIP the placement poll observed (02 §1 — no new poll, no OS event
        // subscription; the 250 ms poll that already detects placement changes is the detector).
        // Compared per pump against the state last seen, not gated on placement_dirty, so a flip is
        // never lost to the dirty bit being consumed by the persistence write above. Gated on
        // `alive` because a dying window's pump can return before its poll ran, and comparing its
        // stale default placement against the adoption baseline would report a flip that never
        // happened — to a window about to be retired anyway.
        const bool maximized = window.last_placement().maximized;
        if (alive && maximized != windows_[i].last_maximized)
        {
            windows_[i].last_maximized = maximized;
            if (chrome_maximized_sink_)
            {
                chrome_maximized_sink_(windows_[i].id, maximized);
            }
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
