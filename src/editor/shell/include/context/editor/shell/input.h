// The Shell's input pump and routing (design 03 §6) — normalize, arbitrate, dispatch.
//
// The whole of 03 §6 except the two OS/CEF calls at either end lives here as PURE logic over
// explicit inputs, so every branch runs in the ctest on every OS. That is deliberate: the OS event
// source is Windows-only in v1 and the sink is CEF (a CI-only dependency path), so arbitration
// written inside either one would be exercised by nothing the local dev gate can run.
//
// The five decisions this makes, in order:
//
//   1. REGION ARBITRATION (§6.2). Editor-core publishes the window's region map — viewport content
//      rects plus "native-interaction" regions — on every layout change. A pointer inside one takes
//      the NATIVE path; everywhere else is the browser's. The map is replaced wholesale, never
//      patched: a layout change that added a panel and moved two others is ONE consistent state, and
//      an incremental update is how a stale rect outlives the panel it belonged to.
//   2. CAPTURE (the `UiInputRouter` shape, `input_routing.h:62-112`, reused rather than re-invented).
//      A capture entry is either MODAL (a miss is swallowed — the dropdown backdrop) or an OVERLAY
//      (a miss falls through to normal arbitration). Pressing a button implicitly captures until the
//      release, which is what makes a drag that leaves the region keep going to where it started.
//   3. DPI. The OS position is physical; a browser mouse event is DIP. Converted once, here.
//   4. FOCUS CLASS (§6.4). A DOM editable having focus sends keys to the browser; otherwise the
//      keymap gets first refusal and UNRESOLVED keys still fall through to the browser. The keymap
//      itself lands with e07, so its resolver is a seam that by default resolves nothing — which
//      makes today's behaviour "everything reaches the browser", the honest v1.
//   5. R-HUX-011 TIMESTAMPS. Stamped at dispatch, on the way out, for the input->commit->derive->paint
//      chain. Passed IN rather than read from a clock so the tests are deterministic.

#pragma once

#include "context/editor/shell/dpi.h"
#include "context/render/rhi.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::shell
{

// What a published region is FOR. Both take the native path; the distinction is which native
// consumer, and it is carried so a caller never re-derives it from an id naming convention.
enum class RegionKind
{
    // A viewport panel's content rect: camera controls, picking, gizmo gestures (03 §6.3).
    viewport,
    // A non-viewport native-interaction region (a native-drawn overlay claiming its own input).
    native,
};

struct ShellRegion
{
    std::string id;
    render::Rect2D rect; // PHYSICAL client pixels, matching what the OS reports
    RegionKind kind = RegionKind::viewport;
};

// The window's published regions. Hit-testing is BACK-TO-FRONT — the LAST matching entry wins —
// mirroring the UI package's `hit_test` (`src/layout.cpp:139-176`), so the publisher expresses
// stacking by order alone and never needs a z field the two sides could disagree about.
class RegionMap
{
public:
    // Replace the whole map (a layout change). See the header note on why this is not incremental.
    void publish(std::vector<ShellRegion> regions);

    [[nodiscard]] const ShellRegion* hit_test(PointI physical) const;
    [[nodiscard]] const ShellRegion* find(const std::string& id) const;
    [[nodiscard]] const std::vector<ShellRegion>& regions() const { return regions_; }
    [[nodiscard]] std::size_t size() const { return regions_.size(); }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }

private:
    std::vector<ShellRegion> regions_;
    // Bumped on every publish, so a layout change is detectable by comparing one integer rather
    // than diffing rect vectors every frame. NOTE: nothing consumes it yet — the compositor damages
    // itself directly from publish_viewports() today. This is the seam e11's viewport-content
    // damage path is expected to read; until then its only readers are the RegionMap tests.
    std::uint64_t generation_ = 0;
};

// ------------------------------------------------------------------------------- normalized events

enum class PointerAction
{
    move,
    down,
    up,
    wheel,
    leave, // the pointer left the client area (CEF needs the explicit mouse-leave)
};

enum class MouseButton
{
    none,
    left,
    middle,
    right,
};

struct Modifiers
{
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool meta = false; // Win/Command
    bool left_button_down = false;
    bool middle_button_down = false;
    bool right_button_down = false;
};

// One normalized pointer sample. Positions are PHYSICAL client pixels — the DIP conversion happens
// at dispatch, so the region map (published in physical pixels) and the raw sample never disagree.
struct PointerEvent
{
    PointerAction action = PointerAction::move;
    PointI position;
    MouseButton button = MouseButton::none;
    Modifiers modifiers;
    std::int32_t wheel_delta_x = 0;
    std::int32_t wheel_delta_y = 0;
    std::int32_t click_count = 1;
};

enum class KeyAction
{
    raw_key_down, // CEF KEYEVENT_RAWKEYDOWN
    key_down,     // CEF KEYEVENT_KEYDOWN
    key_up,       // CEF KEYEVENT_KEYUP
    character,    // CEF KEYEVENT_CHAR
};

struct KeyEvent
{
    KeyAction action = KeyAction::raw_key_down;
    std::int32_t windows_key_code = 0;
    std::int32_t native_key_code = 0;
    char32_t character = 0;
    Modifiers modifiers;
    bool is_system_key = false;
};

// Where editor-core says keyboard focus currently is (§6.4, reported over the bridge).
enum class FocusClass
{
    // Nothing text-editable has focus: the keymap gets first refusal.
    none,
    // A DOM editable has focus: keys are the browser's, unconditionally.
    dom_editable,
};

// ------------------------------------------------------------------------------------- dispatch

enum class InputTarget
{
    browser,   // CEF SendMouse*/SendKey*Event
    viewport,  // the native viewport path (camera / picking / gizmo gestures)
    native,    // a non-viewport native-interaction region
    keymap,    // resolved by the command keymap (e07)
    swallowed, // a modal capture ate it; nothing is dispatched
};

struct PointerDispatch
{
    InputTarget target = InputTarget::browser;
    // The region that claimed it (empty for the browser / swallowed).
    std::string region_id;
    // DIP, window-relative — what a browser mouse event carries.
    PointI logical_position;
    // Physical, REGION-relative — what a viewport's picking/gesture code wants. Signed because a
    // captured drag legitimately leaves the region it started in.
    PointI region_position;
    // R-HUX-011: stamped on the way out, the head of the input->commit->derive->paint chain.
    std::uint64_t dispatch_timestamp_us = 0;
};

struct KeyDispatch
{
    InputTarget target = InputTarget::browser;
    std::uint64_t dispatch_timestamp_us = 0;
};

// A capture entry. `modal` is the `UiInputRouter` capturing/non-capturing distinction: a modal
// capture SWALLOWS a miss (the backdrop behind an open dropdown), an overlay capture lets it fall
// through to normal arbitration.
struct Capture
{
    std::string region_id; // empty == the browser holds the capture
    InputTarget target = InputTarget::browser;
    bool modal = false;
};

// Arbitrates one window's input. Not thread-safe: it is driven from the single shell-owned pump.
class InputArbiter
{
public:
    // The keymap seam (§6.4). Returns true when the command keymap claims `key`, in which case the
    // dispatch target is `keymap`. e07 installs the real resolver; until then nothing is claimed and
    // every unresolved key falls through to the browser — see the header note.
    using KeymapResolver = bool (*)(const KeyEvent&, void* user_data);

    [[nodiscard]] RegionMap& regions() { return regions_; }
    [[nodiscard]] const RegionMap& regions() const { return regions_; }

    void set_dpi(DpiScale scale) { dpi_ = scale; }
    [[nodiscard]] DpiScale dpi() const { return dpi_; }

    void set_focus_class(FocusClass focus) { focus_ = focus; }
    [[nodiscard]] FocusClass focus_class() const { return focus_; }

    void set_keymap_resolver(KeymapResolver resolver, void* user_data);

    // --- explicit captures (a popup opening, a modal dialog) --------------------------------------
    void push_capture(const Capture& capture);
    // Pop the top capture when it names `region_id` (empty matches the browser capture). Returns
    // false when the top is something else — popping blind is how a popup closing tears down the
    // drag capture that opened above it.
    bool pop_capture(const std::string& region_id);
    [[nodiscard]] const Capture* active_capture() const;
    [[nodiscard]] std::size_t capture_depth() const { return captures_.size(); }

    // Drop the IMPLICIT drag capture (the explicit stack is untouched). The window losing focus is
    // the case that needs it: the pointer-up that would have released the drag is going to a
    // different window, so without this the next click here still routes to whatever the drag
    // started on. Idempotent.
    void cancel_pointer_capture();
    [[nodiscard]] bool has_pointer_capture() const { return button_capture_.has_value(); }

    // --- routing ---------------------------------------------------------------------------------
    [[nodiscard]] PointerDispatch route_pointer(const PointerEvent& event, std::uint64_t now_us);
    [[nodiscard]] KeyDispatch route_key(const KeyEvent& event, std::uint64_t now_us);

    // Counters the smoke asserts an input round-trip from.
    [[nodiscard]] int pointer_dispatches() const { return pointer_dispatches_; }
    [[nodiscard]] int key_dispatches() const { return key_dispatches_; }
    [[nodiscard]] int swallowed() const { return swallowed_; }

private:
    // Fill a dispatch's positions from a physical sample + the claiming region (nullptr == browser).
    void fill_positions(PointerDispatch& out, const PointerEvent& event,
                        const ShellRegion* region) const;

    RegionMap regions_;
    DpiScale dpi_;
    FocusClass focus_ = FocusClass::none;
    KeymapResolver keymap_resolver_ = nullptr;
    void* keymap_user_data_ = nullptr;
    std::vector<Capture> captures_;
    // The implicit button capture, distinct from the explicit stack: it is pushed on a press and
    // popped on the matching release, and only ONE can be live (a second button pressed mid-drag
    // does not re-target the drag).
    std::optional<Capture> button_capture_;
    MouseButton button_capture_button_ = MouseButton::none;
    int pointer_dispatches_ = 0;
    int key_dispatches_ = 0;
    int swallowed_ = 0;
};

} // namespace context::editor::shell
