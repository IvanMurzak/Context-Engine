// The Shell-mediated cross-window drag session (M9 e10c) — see cross_window_drag.h for the model and
// the SAFETY-CRITICAL capture-release invariant.

#include "context/editor/shell/cross_window_drag.h"

#include <utility>

namespace context::editor::shell
{

// --------------------------------------------------------------------------- ScopedCursorCapture

ScopedCursorCapture::ScopedCursorCapture(IGlobalCursorCapture& capture) : capture_(&capture)
{
    // Take it now. If the OS refuses, `took_` stays false and `holds()` is false, so the caller
    // refuses to begin a captureless drag.
    took_ = capture_->capture();
}

ScopedCursorCapture::~ScopedCursorCapture()
{
    release();
}

ScopedCursorCapture::ScopedCursorCapture(ScopedCursorCapture&& other) noexcept
    : capture_(other.capture_), took_(other.took_)
{
    // The moved-from guard must own NOTHING, or the capture would be released twice — once by each
    // guard's destructor. `release()` on it is then a no-op.
    other.capture_ = nullptr;
    other.took_ = false;
}

ScopedCursorCapture& ScopedCursorCapture::operator=(ScopedCursorCapture&& other) noexcept
{
    if (this != &other)
    {
        // Release whatever this guard currently holds before adopting the other's, so no capture is
        // silently dropped without a release.
        release();
        capture_ = other.capture_;
        took_ = other.took_;
        other.capture_ = nullptr;
        other.took_ = false;
    }
    return *this;
}

void ScopedCursorCapture::release()
{
    if (capture_ != nullptr && took_)
    {
        capture_->release();
        took_ = false;
    }
}

// --------------------------------------------------------------------------- CrossWindowDragStore

void CrossWindowDragStore::publish_hover(const DragHover& hover)
{
    hover_ = hover;
    // A fresh hover has not been answered yet: drop the previous target's zone so a stale highlight
    // never leaks into a new cursor frame.
    zone_ = DragZone{};
    ++hovers_published_;
}

void CrossWindowDragStore::clear_hover()
{
    hover_ = DragHover{};
    zone_ = DragZone{};
}

DragHover CrossWindowDragStore::hover_for(WindowId reader) const
{
    if (hover_.active && hover_.target == reader)
    {
        return hover_;
    }
    return DragHover{};
}

void CrossWindowDragStore::report_zone(const DragZone& zone)
{
    // A late answer for a hover the cursor has already moved past is stale — the generation guards it.
    if (!hover_.active || zone.generation != hover_.generation)
    {
        return;
    }
    zone_ = zone;
    ++zones_reported_;
}

// --------------------------------------------------------------------------- CrossWindowDragSession

const char* to_string(DragEndReason reason)
{
    switch (reason)
    {
    case DragEndReason::none:
        return "none";
    case DragEndReason::dropped:
        return "dropped";
    case DragEndReason::dropped_no_zone:
        return "dropped-no-zone";
    case DragEndReason::escaped:
        return "escaped";
    case DragEndReason::target_closed:
        return "target-closed";
    case DragEndReason::source_closed:
        return "source-closed";
    case DragEndReason::no_capture:
        return "no-capture";
    }
    return "unknown";
}

CrossWindowDragSession::CrossWindowDragSession(CrossWindowDragStore& store) : store_(store) {}

void CrossWindowDragSession::bind_window_at_point(WindowAtPoint resolver)
{
    window_at_point_ = std::move(resolver);
}

void CrossWindowDragSession::bind_to_local(ToLocal to_local)
{
    to_local_ = std::move(to_local);
}

void CrossWindowDragSession::bind_drop(DropHandler handler)
{
    drop_ = std::move(handler);
}

bool CrossWindowDragSession::begin(WindowId source, PanelSeed seed, PointI screen,
                                   IGlobalCursorCapture& capture)
{
    // Take the OS capture through the RAII guard FIRST. If it could not be taken, END immediately with
    // `no_capture` — never a half-begun, captureless drag (which would leak nothing but also route no
    // pointer events, a silent dead gesture).
    capture_obj_ = &capture;
    capture_guard_.emplace(capture);
    if (!capture_guard_->holds())
    {
        end(DragEndReason::no_capture);
        return false;
    }

    seed_ = std::move(seed);
    source_ = source;
    target_ = kInvalidWindowId;
    ghost_ = screen;
    zone_ = DragZone{};
    end_reason_ = DragEndReason::none;
    active_ = true;
    // Resolve the first hover from the start position.
    update_cursor(screen);
    return true;
}

void CrossWindowDragSession::update_cursor(PointI screen)
{
    if (!active_)
    {
        return;
    }
    // The Shell-owned ghost always follows the global cursor, whether or not a window is under it.
    ghost_ = screen;

    const WindowId over = window_at_point_ ? window_at_point_(screen) : kInvalidWindowId;
    target_ = over;

    DragHover hover;
    ++hover_generation_;
    hover.generation = hover_generation_;
    hover.panel_id = seed_.panel_id;
    if (over != kInvalidWindowId)
    {
        hover.active = true;
        hover.target = over;
        hover.local = to_local_ ? to_local_(over, screen) : screen;
    }
    // Over no live window: an INACTIVE hover, so every window's `drag.probe` answers `active:false`.
    store_.publish_hover(hover);
    // A fresh hover invalidates the last zone until the (possibly new) target answers.
    zone_ = DragZone{};
}

void CrossWindowDragSession::sync_zone()
{
    if (!active_)
    {
        return;
    }
    zone_ = store_.zone();
}

DragEndReason CrossWindowDragSession::drop()
{
    if (!active_)
    {
        return end_reason_;
    }
    // See the latest cross-window answer before deciding.
    sync_zone();

    const bool valid_here = target_ != kInvalidWindowId && zone_.valid &&
                            zone_.generation == hover_generation_;
    if (valid_here)
    {
        const bool ok = drop_ ? drop_(target_, seed_) : false;
        // Even a rehome the handler declined ENDS the drag and releases the capture — a dead drop is
        // still the end of the gesture, never a reason to keep the desktop captured.
        end(ok ? DragEndReason::dropped : DragEndReason::dropped_no_zone);
        return end_reason_;
    }
    // Dropped where there was no valid zone: the panel stays in its source window (no move).
    end(DragEndReason::dropped_no_zone);
    return end_reason_;
}

void CrossWindowDragSession::cancel()
{
    if (!active_)
    {
        return;
    }
    end(DragEndReason::escaped);
}

void CrossWindowDragSession::on_window_closed(WindowId id)
{
    if (!active_)
    {
        return;
    }
    // Order matters: a window that is BOTH source and target (it never is by construction — the drag
    // left the source — but the code must not assume it) is treated as the source dying.
    if (id == source_)
    {
        end(DragEndReason::source_closed);
        return;
    }
    if (id == target_)
    {
        // The window under the cursor died: drop the reference BEFORE ending so nothing reads through
        // it, and end (releasing the capture). CE #319 doubled — the session held a reference across two
        // windows, and this is the one that just went away.
        target_ = kInvalidWindowId;
        end(DragEndReason::target_closed);
        return;
    }
    // An unrelated window closed — the drag continues.
}

bool CrossWindowDragSession::capture_released() const
{
    // Ended AND the real OS capture is down. Reading the capture object (not a cached bool) is what
    // makes this a genuine proof the desktop was freed rather than a flag we promised to flip.
    return !active_ && capture_obj_ != nullptr && !capture_obj_->captured();
}

void CrossWindowDragSession::end(DragEndReason reason)
{
    // Release the OS capture by destroying the guard — the single line the whole safety invariant rests
    // on. `reset()` runs ~ScopedCursorCapture, which calls release(); doing it here, on the ONE path
    // every terminal state routes through, is why no branch can leak it.
    capture_guard_.reset();
    // Stop advertising a hover: the drag is over, so no window should still answer a zone for it.
    store_.clear_hover();
    target_ = kInvalidWindowId;
    active_ = false;
    end_reason_ = reason;
}

} // namespace context::editor::shell
