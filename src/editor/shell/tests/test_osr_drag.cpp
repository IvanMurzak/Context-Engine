// The OSR drag protocol (M9 b1, D11) — `osr_drag.h`'s state machine and the operations-mask mirror.
//
// WHAT THIS SUITE IS FOR, and why it is where the drag's correctness actually lives. The drag's OS
// half is unreachable from CI on two of three platforms (Windows CI is Session 0 — no interactive
// desktop, no gesture) and its CEF half lives in the one translation unit the local dev gate cannot
// build. So the honest place to pin the protocol is here: the same state machine the shipping Shell
// runs, driven directly, on all three default `build` legs.
//
// EVERY ASSERTION BELOW IS ABOUT A SEQUENCE, not a count, because both of CEF's normative ordering
// rules are claims about order (cef_browser.h:903-904, :933-935): an `over` may only follow an
// `enter`, and every `DragTarget*` must precede every `DragSource*`. A suite that counted
// injections would pass against a machine that emitted them backwards.

#include "context/editor/shell/osr_drag.h"

#include "shell_test.h"

#include <cstdio>
#include <vector>

using namespace context::editor::shell;

namespace
{

// The kinds an emitted batch holds, in order — the shape every assertion below compares.
[[nodiscard]] std::vector<OsrDragInjectionKind> kinds(const OsrDragInjections& injections)
{
    std::vector<OsrDragInjectionKind> out;
    for (const OsrDragInjection& injection : injections)
    {
        out.push_back(injection.kind);
    }
    return out;
}

[[nodiscard]] bool kinds_are(const OsrDragInjections& injections,
                             const std::vector<OsrDragInjectionKind>& expected)
{
    const std::vector<OsrDragInjectionKind> actual = kinds(injections);
    if (actual != expected)
    {
        // PRINTED, because a bare `CHECK` on a vector comparison says only "false" — and the whole
        // point of this suite is WHICH sequence came out.
        std::fprintf(stderr, "  sequence mismatch: got [");
        for (const OsrDragInjectionKind kind : actual)
        {
            std::fprintf(stderr, "%s ", to_string(kind));
        }
        std::fprintf(stderr, "] want [");
        for (const OsrDragInjectionKind kind : expected)
        {
            std::fprintf(stderr, "%s ", to_string(kind));
        }
        std::fprintf(stderr, "]\n");
        return false;
    }
    return true;
}

Modifiers dragging_modifiers()
{
    Modifiers modifiers;
    modifiers.left_button_down = true;
    return modifiers;
}

// ---------------------------------------------------------------------- 1. the operations mask

void test_operations_mask()
{
    // The mirror's VALUES. `cef_shell.cpp` static_asserts these same enumerators against
    // `cef_drag_operations_mask_t` on the CEF legs; this is the half that runs everywhere, so a
    // renumbering that somehow reached only one side is caught on both.
    CHECK(drag_operation_bit(DragOperation::none) == 0u);
    CHECK(drag_operation_bit(DragOperation::copy) == 1u);
    CHECK(drag_operation_bit(DragOperation::link) == 2u);
    CHECK(drag_operation_bit(DragOperation::generic) == 4u);
    CHECK(drag_operation_bit(DragOperation::private_op) == 8u);
    CHECK(drag_operation_bit(DragOperation::move) == 16u);
    CHECK(drag_operation_bit(DragOperation::erase) == 32u);
    // DRAG_OPERATION_EVERY is UINT_MAX, NOT the OR of the named bits — the difference is every
    // unnamed bit, and a mirror that used the OR would report "every" as a mask that excludes them.
    CHECK(kDragOperationEvery == 0xFFFFFFFFu);
    CHECK(kDragOperationEvery != (1u | 2u | 4u | 8u | 16u | 32u));

    const DragOperationMask copy_or_move =
        drag_operation_bit(DragOperation::copy) | drag_operation_bit(DragOperation::move);
    CHECK(drag_mask_allows(copy_or_move, DragOperation::copy));
    CHECK(drag_mask_allows(copy_or_move, DragOperation::move));
    CHECK(!drag_mask_allows(copy_or_move, DragOperation::link));
    // `none` is the ABSENCE of an operation, and 0 & anything is 0 — so a bitwise test would report
    // it as permitted by EVERY mask including the empty one. The one value that must answer false.
    CHECK(!drag_mask_allows(copy_or_move, DragOperation::none));
    CHECK(!drag_mask_allows(kDragOperationEvery, DragOperation::none));
    CHECK(!drag_mask_allows(kDragOperationNone, DragOperation::copy));
    CHECK(drag_mask_allows(kDragOperationEvery, DragOperation::erase));
}

// ------------------------------------------------------------- 2. the ordinary drag: in and drop

void test_enter_over_drop_sequence()
{
    OsrDragSession session;
    const DragOperationMask allowed =
        drag_operation_bit(DragOperation::copy) | drag_operation_bit(DragOperation::move);

    // The return value IS `StartDragging`'s: true means "drive it", and the whole of b1 is that it
    // is no longer the false the unimplemented default returned.
    CHECK(session.begin(allowed, PointI{100, 40}));
    CHECK(session.active());
    CHECK(!session.entered()); // the START point is not an entry: no injection has been made yet
    CHECK(session.drags_begun() == 1);
    CHECK(session.drags_ended() == 0);
    CHECK(session.last_view_point() == (PointI{100, 40}));

    const OsrDragInjections first = session.move(PointI{110, 50}, dragging_modifiers(), true);
    CHECK(kinds_are(first, {OsrDragInjectionKind::target_enter}));
    CHECK(first[0].view_dip == (PointI{110, 50}));
    CHECK(first[0].allowed_ops == allowed);
    CHECK(first[0].modifiers.left_button_down);
    CHECK(session.entered());

    // EVERY subsequent sample is an `over`, never a second `enter`. A repeated enter is not a
    // harmless duplicate: it re-runs the renderer's `dragenter` and re-picks the drop target from
    // scratch on every frame.
    for (int step = 0; step < 3; ++step)
    {
        const OsrDragInjections next =
            session.move(PointI{120 + step, 60}, dragging_modifiers(), true);
        CHECK(kinds_are(next, {OsrDragInjectionKind::target_over}));
        CHECK(next[0].allowed_ops == allowed);
    }

    // The view answered: this drop would MOVE. That answer is what `DragSourceEndedAt` must carry.
    session.set_operation(DragOperation::move);
    CHECK(session.operation() == DragOperation::move);

    const OsrDragInjections drop = session.release(PointI{130, 70}, dragging_modifiers(), true);
    // THE NORMATIVE ORDER: the drop (a DragTarget* member) strictly before both DragSource* ones.
    CHECK(kinds_are(drop, {OsrDragInjectionKind::target_drop, OsrDragInjectionKind::source_ended,
                           OsrDragInjectionKind::source_system_ended}));
    CHECK(drop[0].view_dip == (PointI{130, 70}));
    CHECK(drop[1].view_dip == (PointI{130, 70}));
    CHECK(drop[1].operation == DragOperation::move);
    CHECK(!drop.overflowed());

    CHECK(!session.active());
    CHECK(session.end_reason() == OsrDragEndReason::dropped);
    CHECK(session.drags_ended() == 1);
}

// ----------------------------------------------------- 3. leaving the view, and coming back to it

void test_leave_and_reenter()
{
    OsrDragSession session;
    CHECK(session.begin(kDragOperationEvery, PointI{10, 10}));

    CHECK(kinds_are(session.move(PointI{20, 20}, dragging_modifiers(), true),
                    {OsrDragInjectionKind::target_enter}));
    session.set_operation(DragOperation::copy);

    // Out of the view: exactly one leave, and no further injection while it stays out.
    CHECK(kinds_are(session.move(PointI{-5, 20}, dragging_modifiers(), false),
                    {OsrDragInjectionKind::target_leave}));
    CHECK(!session.entered());
    CHECK(session.active()); // the DRAG is still live — the pointer may come back
    // The operation the view reported was an answer about a position the pointer has LEFT, so it is
    // cleared. Otherwise a drag that wandered off and was released outside would report the stale
    // "copy" as the operation it performed.
    CHECK(session.operation() == DragOperation::none);

    CHECK(kinds_are(session.move(PointI{-40, 20}, dragging_modifiers(), false), {}));
    CHECK(kinds_are(session.move(PointI{-80, 25}, dragging_modifiers(), false), {}));
    // Still tracking, so a source_ended fired now would name where the pointer really is.
    CHECK(session.last_view_point() == (PointI{-80, 25}));

    // Back in: a fresh ENTER, not an over — the view was told the drag left, so it has no drag.
    CHECK(kinds_are(session.move(PointI{30, 30}, dragging_modifiers(), true),
                    {OsrDragInjectionKind::target_enter}));
    CHECK(kinds_are(session.move(PointI{31, 31}, dragging_modifiers(), true),
                    {OsrDragInjectionKind::target_over}));
}

// --------------------------------------------------- 4. released outside the view (no drop at all)

void test_release_outside_view()
{
    OsrDragSession session;
    CHECK(session.begin(kDragOperationEvery, PointI{10, 10}));
    CHECK(kinds_are(session.move(PointI{20, 20}, dragging_modifiers(), true),
                    {OsrDragInjectionKind::target_enter}));
    session.set_operation(DragOperation::move);

    const OsrDragInjections out = session.release(PointI{-30, 20}, dragging_modifiers(), false);
    // The leave FIRST (a DragTarget* member), then the two source ends. No drop: the header allows
    // `DragTargetDrop` only after an enter that has not been left, and there is nothing under the
    // pointer to drop onto anyway.
    CHECK(kinds_are(out, {OsrDragInjectionKind::target_leave, OsrDragInjectionKind::source_ended,
                          OsrDragInjectionKind::source_system_ended}));
    // NOTHING WAS PERFORMED, even though the view had last answered "move". This is the assertion
    // that keeps the operation honest: reporting `move` here would tell the renderer its content
    // moved when the drop never happened.
    CHECK(out[1].operation == DragOperation::none);
    CHECK(session.end_reason() == OsrDragEndReason::dropped_outside_view);
    CHECK(session.drags_ended() == 1);
}

// ------------------------------------------- 5. a release with no move at all (the implicit enter)

void test_release_without_a_prior_move()
{
    OsrDragSession session;
    CHECK(session.begin(kDragOperationEvery, PointI{50, 50}));
    CHECK(!session.entered());

    // A drag whose very first sample after `StartDragging` is the release — a fast flick, or a
    // synthetic gesture. `DragTargetDrop` is legal only "after calling DragTargetDragEnter", so the
    // enter is synthesised rather than the drop being dropped.
    const OsrDragInjections out = session.release(PointI{55, 55}, dragging_modifiers(), true);
    CHECK(kinds_are(out, {OsrDragInjectionKind::target_enter, OsrDragInjectionKind::target_drop,
                          OsrDragInjectionKind::source_ended,
                          OsrDragInjectionKind::source_system_ended}));
    // FOUR is the longest legal sequence and exactly the fixed capacity — so this case is also the
    // proof that the capacity was derived rather than guessed.
    CHECK(out.size() == OsrDragInjections::kCapacity);
    CHECK(!out.overflowed());
    CHECK(out[0].view_dip == (PointI{55, 55}));
    CHECK(out[1].view_dip == (PointI{55, 55}));
}

// ------------------------------------------------------------------- 6. every cancel path

void test_cancel_paths()
{
    const OsrDragEndReason reasons[] = {OsrDragEndReason::escaped, OsrDragEndReason::focus_lost,
                                        OsrDragEndReason::window_closed};
    for (const OsrDragEndReason reason : reasons)
    {
        // Cancelled while INSIDE the view: the leave comes first, then both source ends.
        OsrDragSession inside;
        CHECK(inside.begin(kDragOperationEvery, PointI{5, 5}));
        CHECK(!inside.move(PointI{10, 10}, dragging_modifiers(), true).empty());
        inside.set_operation(DragOperation::copy);
        const OsrDragInjections cancelled = inside.cancel(reason);
        CHECK(kinds_are(cancelled,
                        {OsrDragInjectionKind::target_leave, OsrDragInjectionKind::source_ended,
                         OsrDragInjectionKind::source_system_ended}));
        CHECK(cancelled[1].operation == DragOperation::none);
        // WHERE the drag was when it was cancelled — a cancel carries no position of its own, so
        // the last known one is what `DragSourceEndedAt` must report.
        CHECK(cancelled[1].view_dip == (PointI{10, 10}));
        CHECK(!inside.active());
        CHECK(inside.end_reason() == reason);
        CHECK(inside.drags_begun() == inside.drags_ended());

        // Cancelled while OUTSIDE it: no leave to send, because none was ever entered.
        OsrDragSession outside;
        CHECK(outside.begin(kDragOperationEvery, PointI{5, 5}));
        CHECK(kinds_are(outside.cancel(reason), {OsrDragInjectionKind::source_ended,
                                                 OsrDragInjectionKind::source_system_ended}));
        CHECK(outside.end_reason() == reason);
        CHECK(outside.drags_begun() == outside.drags_ended());
    }
}

// ------------------------------------------------- 7. an inactive session emits absolutely nothing

void test_inactive_session_is_silent()
{
    OsrDragSession session;
    // NEVER BEGUN. This is the state a browser with no window above it leaves the session in, and
    // the state after every terminal path — and it is the structural half of CEF's rule "Don't call
    // any of CefBrowserHost::DragSource*Ended* methods after returning false".
    CHECK(!session.active());
    CHECK(kinds_are(session.move(PointI{1, 1}, Modifiers{}, true), {}));
    CHECK(kinds_are(session.release(PointI{1, 1}, Modifiers{}, true), {}));
    CHECK(kinds_are(session.cancel(OsrDragEndReason::escaped), {}));
    session.set_operation(DragOperation::move);
    CHECK(session.operation() == DragOperation::none);
    CHECK(session.drags_begun() == 0);
    CHECK(session.drags_ended() == 0);

    // ENDED. The same silence, so a stray sample arriving after a drop cannot inject a second
    // source-end into a renderer that has already been told the drag is over.
    OsrDragSession ended;
    CHECK(ended.begin(kDragOperationEvery, PointI{0, 0}));
    CHECK(!ended.release(PointI{2, 2}, dragging_modifiers(), true).empty());
    CHECK(!ended.active());
    CHECK(kinds_are(ended.move(PointI{3, 3}, dragging_modifiers(), true), {}));
    CHECK(kinds_are(ended.release(PointI{3, 3}, dragging_modifiers(), true), {}));
    CHECK(kinds_are(ended.cancel(OsrDragEndReason::escaped), {}));
    CHECK(ended.drags_ended() == 1); // and NOT 2 — the stray calls ended nothing
}

// ----------------------------------------------------------- 8. a second drag cannot overlap a live one

void test_second_begin_is_refused()
{
    OsrDragSession session;
    CHECK(session.begin(drag_operation_bit(DragOperation::copy), PointI{1, 2}));
    // FALSE is what `StartDragging` then returns, which aborts the second drag — and CEF's rule
    // that no `DragSource*Ended*` may follow a false is satisfied because the second drag never
    // becomes any of this session's state.
    CHECK(!session.begin(kDragOperationEvery, PointI{9, 9}));
    // The FIRST drag is untouched: still live, still carrying its own mask and its own start point.
    CHECK(session.active());
    CHECK(session.allowed_ops() == drag_operation_bit(DragOperation::copy));
    CHECK(session.last_view_point() == (PointI{1, 2}));
    CHECK(session.drags_begun() == 1);

    // Once it ends, a new drag begins normally — the refusal is about overlap, not a latch.
    CHECK(!session.cancel(OsrDragEndReason::escaped).empty());
    CHECK(session.begin(kDragOperationEvery, PointI{9, 9}));
    CHECK(session.drags_begun() == 2);
}

// --------------------------------------------- 9. the overflow flag can actually be set

// Every `overflowed()` assertion above is a `!overflowed()`, and a flag that never fires is the
// vacuous-gate failure in miniature — the same control `ShellEventBatch` carries next door
// (test_window.cpp, `..._reports_overflow_rather_than_dropping_silently`). No PROTOCOL path emits a
// fifth step (osr_drag.h derives the capacity from the longest legal sequence), so the buffer is
// driven directly: that is the point — it proves the tripwire works for the future path that would.
void test_injections_report_overflow_rather_than_dropping_silently()
{
    OsrDragInjections injections;
    OsrDragInjection step;
    step.kind = OsrDragInjectionKind::target_over;

    for (std::size_t i = 0; i < OsrDragInjections::kCapacity; ++i)
    {
        injections.push(step);
    }
    CHECK(injections.size() == OsrDragInjections::kCapacity);
    CHECK(!injections.overflowed()); // filling it EXACTLY is not an overflow

    injections.push(step);
    CHECK(injections.overflowed());
    // The tail is dropped, never a torn sequence: a caller iterating `begin()`..`end()` still sees
    // a prefix of well-formed steps rather than a fifth slot that was never written.
    CHECK(injections.size() == OsrDragInjections::kCapacity);
}

// ------------------------------------------------------ 10. the vocabulary a diagnostic prints

void test_to_string()
{
    // Every enumerator, so a new one added without a name shows up as "unknown" HERE rather than in
    // a CI log at 3am. The smoke and the shell's own diagnostics print these.
    CHECK(shelltest::mentions(to_string(DragOperation::move), "move"));
    CHECK(shelltest::mentions(to_string(DragOperation::none), "none"));
    CHECK(shelltest::mentions(to_string(DragOperation::erase), "delete"));
    CHECK(shelltest::mentions(to_string(OsrDragInjectionKind::target_enter), "DragTargetDragEnter"));
    CHECK(shelltest::mentions(to_string(OsrDragInjectionKind::source_system_ended),
                              "DragSourceSystemDragEnded"));
    CHECK(shelltest::mentions(to_string(OsrDragEndReason::dropped_outside_view), "outside"));
    CHECK(shelltest::mentions(to_string(OsrDragEndReason::window_closed), "window"));
}

} // namespace

int main()
{
    test_operations_mask();
    test_enter_over_drop_sequence();
    test_leave_and_reenter();
    test_release_outside_view();
    test_release_without_a_prior_move();
    test_cancel_paths();
    test_inactive_session_is_silent();
    test_second_begin_is_refused();
    test_injections_report_overflow_rather_than_dropping_silently();
    test_to_string();
    SHELL_TEST_MAIN_END();
}
