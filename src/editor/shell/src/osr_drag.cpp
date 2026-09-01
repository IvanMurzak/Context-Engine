// The OSR drag protocol's state machine — see osr_drag.h for the contract and the header citations.

#include "context/editor/shell/osr_drag.h"

namespace context::editor::shell
{

const char* to_string(DragOperation operation)
{
    switch (operation)
    {
    case DragOperation::none:
        return "none";
    case DragOperation::copy:
        return "copy";
    case DragOperation::link:
        return "link";
    case DragOperation::generic:
        return "generic";
    case DragOperation::private_op:
        return "private";
    case DragOperation::move:
        return "move";
    case DragOperation::erase:
        return "delete";
    }
    return "unknown";
}

DragCursor drag_cursor_for(DragOperation operation)
{
    switch (operation)
    {
    case DragOperation::copy:
        return DragCursor::copy;
    case DragOperation::link:
        return DragCursor::link;
    case DragOperation::move:
        return DragCursor::move;
    case DragOperation::generic:
    case DragOperation::private_op:
        // A drop that WILL happen but whose kind the renderer did not name. `move` is the honest
        // read for this Shell's one real producer — a Dockview tab leaves the group it came from —
        // and it is emphatically not `refused`, which is the answer that matters to get right.
        return DragCursor::move;
    case DragOperation::erase:
        // Chromium's `DRAG_OPERATION_DELETE` (a drop that destroys the source). No stock cursor on
        // any of the three platforms says that, and inventing one would be worse than the honest
        // "something will happen here" — the refusal cursor would actively lie.
        return DragCursor::move;
    case DragOperation::none:
    default:
        // ⚠ NOT `DragCursor::none` — see osr_drag.h. This callback only fires DURING a drag, so
        // "no operation" means the pointer is over something that will not take the drop.
        return DragCursor::refused;
    }
}

const char* to_string(DragCursor cursor)
{
    switch (cursor)
    {
    case DragCursor::none:
        return "none";
    case DragCursor::refused:
        return "refused";
    case DragCursor::copy:
        return "copy";
    case DragCursor::link:
        return "link";
    case DragCursor::move:
        return "move";
    }
    return "unknown";
}

const char* to_string(OsrDragInjectionKind kind)
{
    switch (kind)
    {
    case OsrDragInjectionKind::target_enter:
        return "DragTargetDragEnter";
    case OsrDragInjectionKind::target_over:
        return "DragTargetDragOver";
    case OsrDragInjectionKind::target_leave:
        return "DragTargetDragLeave";
    case OsrDragInjectionKind::target_drop:
        return "DragTargetDrop";
    case OsrDragInjectionKind::source_ended:
        return "DragSourceEndedAt";
    case OsrDragInjectionKind::source_system_ended:
        return "DragSourceSystemDragEnded";
    }
    return "unknown";
}

const char* to_string(OsrDragEndReason reason)
{
    switch (reason)
    {
    case OsrDragEndReason::none:
        return "none";
    case OsrDragEndReason::dropped:
        return "dropped";
    case OsrDragEndReason::dropped_outside_view:
        return "dropped-outside-view";
    case OsrDragEndReason::escaped:
        return "escaped";
    case OsrDragEndReason::focus_lost:
        return "focus-lost";
    case OsrDragEndReason::window_closed:
        return "window-closed";
    }
    return "unknown";
}

void OsrDragInjections::push(const OsrDragInjection& injection)
{
    if (count_ >= kCapacity)
    {
        // Unreachable by the capacity derivation in osr_drag.h. RECORDED rather than asserted so
        // the claim is testable on every leg without a debug build: dropping a step silently would
        // leave the renderer mid-drag with no way to tell from the outside.
        overflowed_ = true;
        return;
    }
    items_[count_] = injection;
    ++count_;
}

bool OsrDragSession::begin(DragOperationMask allowed_ops, PointI start_view_dip)
{
    if (active_)
    {
        // The one abort reason (osr_drag.h). Returning false here is what the binding returns from
        // `StartDragging`, and the header forbids any `DragSource*Ended*` call after that — which
        // holds by construction, because the SECOND drag never becomes this session's state and the
        // FIRST one's end is still its own.
        return false;
    }
    active_ = true;
    entered_ = false;
    allowed_ops_ = allowed_ops;
    operation_ = DragOperation::none;
    last_point_ = start_view_dip;
    end_reason_ = OsrDragEndReason::none;
    ++begun_;
    return true;
}

OsrDragInjections OsrDragSession::move(PointI view_dip, Modifiers modifiers, bool inside_view)
{
    OsrDragInjections out;
    if (!active_)
    {
        return out;
    }
    last_point_ = view_dip;

    if (inside_view)
    {
        OsrDragInjection step;
        // ENTER exactly once per entry, OVER for every sample after it. The header orders them
        // ("after calling DragTargetDragEnter and before calling DragTargetDragLeave/DragTargetDrop",
        // cef_browser.h:903-904), and a repeated enter is not a harmless duplicate: it re-runs the
        // renderer's dragenter and re-evaluates the drop target from scratch every frame.
        step.kind = entered_ ? OsrDragInjectionKind::target_over : OsrDragInjectionKind::target_enter;
        step.view_dip = view_dip;
        step.modifiers = modifiers;
        step.allowed_ops = allowed_ops_;
        out.push(step);
        entered_ = true;
        return out;
    }

    if (entered_)
    {
        OsrDragInjection step;
        step.kind = OsrDragInjectionKind::target_leave;
        out.push(step);
        entered_ = false;
        // The operation the view last reported was an answer about a position the pointer has now
        // left, so it is no longer true. Cleared rather than kept, because it is what a subsequent
        // `DragSourceEndedAt` would report as the operation the drag PERFORMED.
        operation_ = DragOperation::none;
    }
    // Outside the view and not entered: nothing to inject. The drag stays live — the pointer may
    // come back — and the session keeps tracking the position for the eventual source_ended.
    return out;
}

OsrDragInjections OsrDragSession::release(PointI view_dip, Modifiers modifiers, bool inside_view)
{
    OsrDragInjections out;
    if (!active_)
    {
        return out;
    }
    last_point_ = view_dip;

    DragOperation performed = DragOperation::none;
    if (inside_view)
    {
        if (!entered_)
        {
            // A drop with no prior enter is illegal ("after calling DragTargetDragEnter",
            // cef_browser.h:921-923), and it is REACHABLE: a drag whose very first sample after
            // `StartDragging` is the release (a fast flick, or a synthetic gesture) has never been
            // told the pointer entered. The implicit enter is what keeps the protocol well-formed
            // instead of dropping the drop.
            OsrDragInjection enter;
            enter.kind = OsrDragInjectionKind::target_enter;
            enter.view_dip = view_dip;
            enter.modifiers = modifiers;
            enter.allowed_ops = allowed_ops_;
            out.push(enter);
            entered_ = true;
        }
        OsrDragInjection drop;
        drop.kind = OsrDragInjectionKind::target_drop;
        drop.view_dip = view_dip;
        drop.modifiers = modifiers;
        out.push(drop);
        // What the drag DID, per the view's own last answer. A drop the view declined to handle
        // leaves this `none`, which is the honest report.
        performed = operation_;
    }
    else if (entered_)
    {
        // Released outside after having been inside: the view must be told the drag left before the
        // source end, or it keeps a drop target highlighted for a drag that is over.
        OsrDragInjection leave;
        leave.kind = OsrDragInjectionKind::target_leave;
        out.push(leave);
        entered_ = false;
    }

    // ALL DragTarget* BEFORE ALL DragSource* (cef_browser.h:933-935) — the view is both the source
    // and the target here, so this ordering is normative, not a preference.
    OsrDragInjection ended;
    ended.kind = OsrDragInjectionKind::source_ended;
    ended.view_dip = view_dip;
    ended.operation = performed;
    out.push(ended);

    OsrDragInjection system_ended;
    system_ended.kind = OsrDragInjectionKind::source_system_ended;
    out.push(system_ended);

    end(inside_view ? OsrDragEndReason::dropped : OsrDragEndReason::dropped_outside_view);
    return out;
}

OsrDragInjections OsrDragSession::cancel(OsrDragEndReason reason)
{
    OsrDragInjections out;
    if (!active_)
    {
        return out;
    }
    if (entered_)
    {
        OsrDragInjection leave;
        leave.kind = OsrDragInjectionKind::target_leave;
        out.push(leave);
        entered_ = false;
    }
    // `DRAG_OPERATION_NONE` on every cancel: nothing was performed. The header allows
    // `DragSourceSystemDragEnded` alone ("may be called immediately without first calling
    // DragSourceEndedAt to cancel a drag operation", cef_browser.h:943-945) — both are sent anyway,
    // so a cancelled drag and a declined drop leave the renderer in the SAME state by the same
    // sequence, which is one path to keep working rather than two.
    OsrDragInjection ended;
    ended.kind = OsrDragInjectionKind::source_ended;
    ended.view_dip = last_point_;
    ended.operation = DragOperation::none;
    out.push(ended);

    OsrDragInjection system_ended;
    system_ended.kind = OsrDragInjectionKind::source_system_ended;
    out.push(system_ended);

    end(reason);
    return out;
}

void OsrDragSession::set_operation(DragOperation operation)
{
    if (!active_)
    {
        return;
    }
    operation_ = operation;
}

void OsrDragSession::end(OsrDragEndReason reason)
{
    active_ = false;
    entered_ = false;
    allowed_ops_ = kDragOperationNone;
    operation_ = DragOperation::none;
    end_reason_ = reason;
    ++ended_;
}

} // namespace context::editor::shell
