// The OSR DRAG PROTOCOL (M9 b1, decision D11; the audit rows it closes are in docs/shell.md § 16).
//
// WHAT WAS BROKEN. `CefRenderHandler::StartDragging` was not overridden, and the pinned header
// defines its default `return false` as *"Return false to abort the drag operation"*
// (cef_render_handler.h:199). So every HTML5 drag in the editor was not merely unhandled — it
// was ACTIVELY REFUSED at the first `dragstart`, which is why Dockview's tab drag, its drop-to-split
// and its re-docking all appeared dead while the bundle's own handlers were present and correct.
// Nothing in editor-core had to change to fix it; the events simply never arrived.
//
// THE PROTOCOL, as the pinned headers define it (CEF 149.0.6+g0d0eeb6+chromium-149.0.7827.201,
// `tools/cef-prebuilt.json`; every line number below was re-derived from that exact distribution —
// see the ⚠ note at the end of this block):
//
//   1. The renderer starts a drag -> `StartDragging(browser, drag_data, allowed_ops, x, y)`
//      (cef_render_handler.h:208). `(x, y)` is the start point in SCREEN coordinates. Returning
//      TRUE means "I will drive this drag"; returning false aborts it.
//   2. While the drag runs, the host feeds the view: `DragTargetDragEnter` (cef_browser.h:897) once
//      on entry, `DragTargetDragOver` (:908) on every move, `DragTargetDragLeave` (:917) when the
//      pointer leaves. All three take VIEW coordinates (a `CefMouseEvent`), never screen ones.
//   3. The view answers with `UpdateDragCursor(browser, operation)` (cef_render_handler.h:222) —
//      the ONE operation it would perform at the current position, which is the drag feedback.
//   4. A drop is `DragTargetDrop` (cef_browser.h:927); the drag then ENDS with `DragSourceEndedAt`
//      (:939 — "|x| and |y| are mouse coordinates relative to the upper-left corner of the view")
//      and `DragSourceSystemDragEnded` (:951).
//
// TWO ORDERING RULES ARE NORMATIVE, and both are enforced here rather than left to a call site:
//
//   * "If the web view is both the drag source and the drag target then all DragTarget* methods
//     should be called before DragSource* mthods" (cef_browser.h:933-935, :945-947) — which is
//     EXACTLY our case: the editor is one document, so a Dockview tab drag is source and target at
//     once. `release()` therefore emits drop -> source_ended -> system_ended in that order, always.
//   * "Don't call any of CefBrowserHost::DragSource*Ended* methods after returning false" from
//     `StartDragging` (cef_render_handler.h:199-200). Structural here: `begin()` returning false
//     leaves the session INACTIVE, and every method of an inactive session emits nothing — so the
//     forbidden call is unreachable rather than merely avoided.
//
// WHY THIS IS A CEF-FREE STATE MACHINE, and not `if`s inside the binding. `cef_shell.cpp` is the one
// translation unit the local dev gate cannot build (CLAUDE.md § Windows local-dev nuance) and the
// only one no headless CI job executes; the Shell's whole layering exists to keep judgement out of
// it. So the binding does exactly two things for the drag — it TRANSLATES `StartDragging` /
// `UpdateDragCursor` into this vocabulary, and it APPLIES the injections this session emits — while
// every decision (has the pointer entered the view? is a drop legal here? which of the six
// injections, in what order, with which operation?) is decided in this file, compiled and executed
// by `editor-shell-test_osr_drag` on all three default `build` legs.
//
// ⚠ THE SHELL OWNS THE DRAG LOOP; IT DOES NOT ENTER THE OS'S. The OS-native outbound transfer
// (Win32 `DoDragDrop`, X11 XDND's own event loop, `NSDraggingSession`) is a MODAL loop that does not
// return until the gesture ends, and this Shell runs CEF on ONE thread with an external message pump
// (`multi_threaded_message_loop = false` + `OnScheduleMessagePumpWork`, design 03 §1) driven by the
// owner loop — the very loop such a modal call would block. Entering it from inside `StartDragging`
// would freeze the browser for the whole drag: no repaint, no `UpdateDragCursor`, no drop feedback.
// The Shell therefore drives the protocol from the pointer stream it ALREADY owns (`EditorWindow`),
// which is what an in-document drag — Dockview's tabs, D11's stated goal — needs and all it needs.
// What that deliberately does NOT yet deliver is a drag that crosses into ANOTHER APPLICATION (a tab
// dropped on the desktop, a file dragged in from a file manager); `docs/shell.md` § 11 carries that
// as a named gap with the mechanism each platform would use, rather than letting a half-entered
// modal loop pretend to it.
//
// ⚠ LAYERED WITH THE SHELL-MEDIATED CROSS-WINDOW DRAG (`cross_window_drag.h`, e10c), NOT AN
// ALTERNATIVE TO IT. That session moves a PANEL between two Shell WINDOWS and owns the global OS
// cursor capture; this one moves DOM content inside ONE window's document and takes no capture at
// all. `drag.ts`'s own fact 2 states the split from the JS side ("IN-WINDOW DOCKVIEW DnD IS
// UNTOUCHED"), and it stays true: nothing here publishes a cross-window hover, and nothing in
// `cross_window_drag.cpp` emits an injection.
//
// ⚠ EVERY `cef_*.h:<line>` CITATION ABOVE WAS RE-DERIVED from the pinned distribution rather than
// copied. The numbers that circulated with this task's own spec (`cef_browser.h:889-927` /
// `:930-946`) are 8-9 lines short of the real declarations — they name the DOC COMMENT, not the
// member. `docs/shell.md` § 16's drag rows carry the corrected `:897-927` / `:939-951`.

#pragma once

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/input.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace context::editor::shell
{

// ------------------------------------------------------------------ the operations mask (mirror)

// The CEF-FREE MIRROR of `cef_drag_operations_mask_t`, under the same discipline as `ext_scheme.h`'s
// scheme-option mirror: the values are pinned by a unit test on all three default `build` legs —
// where CEF does not exist — and `cef_shell.cpp` carries `static_assert`s comparing every enumerator
// against the real header, so a CEF bump that renumbered the mask fails the CEF build LOUDLY instead
// of quietly turning a "copy" drag into a "link" one.
enum class DragOperation : std::uint32_t
{
    none = 0,     // DRAG_OPERATION_NONE
    copy = 1,     // DRAG_OPERATION_COPY
    link = 2,     // DRAG_OPERATION_LINK
    generic = 4,  // DRAG_OPERATION_GENERIC
    private_op = 8,  // DRAG_OPERATION_PRIVATE
    move = 16,    // DRAG_OPERATION_MOVE
    erase = 32,   // DRAG_OPERATION_DELETE — spelled `erase` because `delete` is a keyword
};

// A SET of the above. CEF passes `allowed_ops` as a mask and a single `operation` as one value, and
// the two are different types there in name only (`DragOperationsMask` / `DragOperation` are both
// the same enum), which is exactly how a mask ends up in a single-value slot. Keeping them distinct
// here — an unsigned mask vs the enum — makes that swap a compile error on every leg.
using DragOperationMask = std::uint32_t;

inline constexpr DragOperationMask kDragOperationNone = 0u;
// DRAG_OPERATION_EVERY is `UINT_MAX`, NOT the OR of the named bits. Spelled as the full mask so a
// future enumerator cannot silently fall outside "every".
inline constexpr DragOperationMask kDragOperationEvery = 0xFFFFFFFFu;

[[nodiscard]] constexpr DragOperationMask drag_operation_bit(DragOperation operation)
{
    return static_cast<DragOperationMask>(operation);
}

[[nodiscard]] constexpr bool drag_mask_allows(DragOperationMask mask, DragOperation operation)
{
    // `none` is the ABSENCE of an operation, not a bit: `DRAG_OPERATION_NONE == 0`, so a bitwise
    // test for it is true of every mask, including an empty one. Answering false keeps
    // "is this operation permitted" meaningful for the one value that is not an operation.
    return operation != DragOperation::none && (mask & drag_operation_bit(operation)) != 0u;
}

[[nodiscard]] const char* to_string(DragOperation operation);

// ------------------------------------------------------------------------- the feedback cursor

// What the OS backend should show while the pointer moves (`IWindowBackend::set_drag_cursor`).
//
// A SEPARATE VOCABULARY FROM `DragOperation`, and the reason is a genuine ambiguity in CEF's:
// `DRAG_OPERATION_NONE` means TWO different things depending on when it arrives. From
// `UpdateDragCursor` DURING a drag it means "the thing under the pointer will not accept this
// drop" — the no-drop cursor, and the single most useful piece of feedback the whole protocol
// carries. At the END of a drag the Shell has to say "there is no drag any more, put the ordinary
// pointer back". A backend handed a bare `DragOperation::none` cannot tell those apart, and the
// one it guesses wrong is the one a user sees: either a no-drop cursor stuck on the editor after
// every drag, or no refusal feedback at all during one.
enum class DragCursor
{
    none,    // no drag in flight — the ordinary pointer
    refused, // a live drag over something that will not take it (CEF's DRAG_OPERATION_NONE)
    copy,
    link,
    move,
};

// The cursor for an operation reported by `UpdateDragCursor` — i.e. WHILE A DRAG IS LIVE, which is
// the only context this callback fires in. `DragOperation::none` therefore maps to `refused`, never
// to `none`; the end-of-drag `none` is pushed by the Shell directly and does not come through here.
[[nodiscard]] DragCursor drag_cursor_for(DragOperation operation);

// --------------------------------------------------------------------------- one injection

// Which of the six windowless-only `CefBrowserHost` drag members an emitted step is.
//
// There is deliberately NO `none` member: an `OsrDragInjections` reports its own length, so an
// "empty" injection would be a second way to say the same thing and the first way a caller could
// forget to check. The unused tail of the fixed-capacity buffer is never read.
enum class OsrDragInjectionKind
{
    target_enter,        // CefBrowserHost::DragTargetDragEnter (cef_browser.h:897)
    target_over,         // CefBrowserHost::DragTargetDragOver  (:908)
    target_leave,        // CefBrowserHost::DragTargetDragLeave (:917)
    target_drop,         // CefBrowserHost::DragTargetDrop      (:927)
    source_ended,        // CefBrowserHost::DragSourceEndedAt   (:939)
    source_system_ended, // CefBrowserHost::DragSourceSystemDragEnded (:951)
};

[[nodiscard]] const char* to_string(OsrDragInjectionKind kind);

// ONE step of the protocol, as a value. Every field is meaningful for some kinds and ignored for
// others, which is stated per field rather than split across six structs: the consumer is a single
// `switch` in the binding, and six payload types would make that switch six conversions.
struct OsrDragInjection
{
    // Defaulted to `target_leave` only because `std::array` value-initializes its whole storage;
    // slots past `OsrDragInjections::size()` are never read, so the default carries no meaning.
    OsrDragInjectionKind kind = OsrDragInjectionKind::target_leave;
    // VIEW coordinates, DIP — for `target_enter` / `target_over` / `target_drop` (the
    // `CefMouseEvent`) and for `source_ended` (which the header defines in the same space).
    // Ignored by `target_leave` and `source_system_ended`, which take no coordinates at all.
    PointI view_dip;
    // The `CefMouseEvent` modifier state, for the three that carry one.
    Modifiers modifiers;
    // `allowed_ops`, for `target_enter` / `target_over` only.
    DragOperationMask allowed_ops = kDragOperationNone;
    // The single operation, for `source_ended` only — what the drag actually DID.
    DragOperation operation = DragOperation::none;
};

// The ordered steps one gesture event produces.
//
// FIXED CAPACITY, DERIVED NOT PICKED: the longest legal sequence is the drop of a drag whose pointer
// had not yet entered the view — an implicit `target_enter` (the header requires `DragTargetDrop`
// to follow a `DragTargetDragEnter`), the `target_drop`, then the two `DragSource*` steps the
// ordering rule puts last. That is four, and no path can produce a fifth. Fixed rather than a
// `std::vector` because `move()` runs at cursor frame rate on the owner thread, where a per-sample
// heap allocation is exactly the cost the Shell avoids everywhere else.
class OsrDragInjections
{
public:
    static constexpr std::size_t kCapacity = 4;

    void push(const OsrDragInjection& injection);

    [[nodiscard]] std::size_t size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] const OsrDragInjection& operator[](std::size_t index) const
    {
        return items_[index];
    }
    [[nodiscard]] const OsrDragInjection* begin() const { return items_.data(); }
    [[nodiscard]] const OsrDragInjection* end() const { return items_.data() + count_; }
    // True iff a `push` was ever DROPPED for want of capacity. Unreachable by the derivation above,
    // and exposed anyway so the claim is a tested fact rather than a comment: a future path that
    // emits a fifth step reddens `editor-shell-test_osr_drag` instead of silently losing a step.
    [[nodiscard]] bool overflowed() const { return overflowed_; }

private:
    std::array<OsrDragInjection, kCapacity> items_{};
    std::size_t count_ = 0;
    bool overflowed_ = false;
};

// ------------------------------------------------------------------------------ the session

// Why a drag ended. Everything except `none` is terminal.
enum class OsrDragEndReason
{
    none,                 // still running, or never begun
    dropped,              // released over the view -> DragTargetDrop was injected
    dropped_outside_view, // released with the pointer outside the view -> no drop; the source is
                          // still told the drag ended, which is what un-sticks the renderer
    escaped,              // Escape, or an explicit abort
    focus_lost,           // the window lost focus mid-drag: the release will go somewhere else
    window_closed,        // the window went away mid-drag
};

[[nodiscard]] const char* to_string(OsrDragEndReason reason);

// The protocol run of ONE web-view drag.
//
// Deliberately shaped as "gesture in, injections out" rather than as a thing that calls a browser:
// it makes every ordering rule above a VALUE a test can compare, on the legs where no browser
// exists, and it keeps the session ignorant of `IBrowserHost` (which would otherwise be a link edge
// from the state machine into the seam that carries the CEF implementation).
//
// Not thread-safe, like everything else on the owner loop.
class OsrDragSession
{
public:
    // The web view began a drag (`StartDragging`). `start_view_dip` is the gesture's start point
    // ALREADY CONVERTED from the screen coordinates the header reports, through `osr_view_point`
    // (dpi.h) — the conversion happens where the platform's screen convention is known (the CEF
    // binding, which already holds the client origin and the DPI for `GetScreenPoint`), so this
    // session speaks one unit throughout.
    //
    // RETURNS WHAT `StartDragging` MUST RETURN. False means "abort this drag", and the only reason
    // to abort is that a drag is already live: a second overlapping run would interleave two
    // protocols on one view, and CEF gives no way to tell their `DragSource*` ends apart. Refusing
    // the second is what keeps the invariant "at most one live drag per view" structural.
    [[nodiscard]] bool begin(DragOperationMask allowed_ops, PointI start_view_dip);

    // The pointer moved to `view_dip`. `inside_view` is the caller's own hit test against the
    // browser's view rect — it is NOT derived from `view_dip` here, because the view's extent is
    // the window's to know and a session that guessed it would be wrong the moment a window resized
    // mid-drag.
    [[nodiscard]] OsrDragInjections move(PointI view_dip, Modifiers modifiers, bool inside_view);

    // The drag button came up at `view_dip`. Terminal on every branch.
    [[nodiscard]] OsrDragInjections release(PointI view_dip, Modifiers modifiers, bool inside_view);

    // Abort. `reason` is one of `escaped` / `focus_lost` / `window_closed` — the two `dropped*`
    // reasons belong to `release()`, which is the only path that can inject a drop.
    [[nodiscard]] OsrDragInjections cancel(OsrDragEndReason reason);

    // The view answered `UpdateDragCursor`: the operation it would perform right now. Recorded, and
    // reported back as `DragSourceEndedAt`'s `op` when the drag ends in a drop — which is how the
    // renderer learns whether its own drop moved or copied. Ignored when no drag is live.
    void set_operation(DragOperation operation);

    [[nodiscard]] bool active() const { return active_; }
    // Whether the view currently holds the drag (a `DragTargetDragEnter` has been injected and no
    // `DragTargetDragLeave` since). The re-entry gate: an `over` may only follow an `enter`.
    [[nodiscard]] bool entered() const { return entered_; }
    [[nodiscard]] DragOperationMask allowed_ops() const { return allowed_ops_; }
    [[nodiscard]] DragOperation operation() const { return operation_; }
    // The last point the session was told about, in view DIP — what `DragSourceEndedAt` reports.
    [[nodiscard]] PointI last_view_point() const { return last_point_; }
    [[nodiscard]] OsrDragEndReason end_reason() const { return end_reason_; }
    // How many drags this session has BEGUN and how many it has ENDED. Counters rather than a bool
    // because the honest assertion for the whole feature is a pair: a drag that starts and never
    // ends leaves the renderer stuck in a drag forever, and only the second number can see that.
    [[nodiscard]] std::uint64_t drags_begun() const { return begun_; }
    [[nodiscard]] std::uint64_t drags_ended() const { return ended_; }

private:
    // THE ONE terminal path — mirrors `CrossWindowDragSession::end` next door, for the same reason:
    // "every end does the same bookkeeping" is one auditable line rather than a promise spread over
    // five branches.
    void end(OsrDragEndReason reason);

    PointI last_point_;
    DragOperationMask allowed_ops_ = kDragOperationNone;
    DragOperation operation_ = DragOperation::none;
    OsrDragEndReason end_reason_ = OsrDragEndReason::none;
    std::uint64_t begun_ = 0;
    std::uint64_t ended_ = 0;
    bool active_ = false;
    bool entered_ = false;
};

} // namespace context::editor::shell
