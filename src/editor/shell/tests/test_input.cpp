// Input arbitration (03 §6): region hit-testing, the capture stack (modal swallow vs overlay
// fall-through), the implicit drag capture, DPI-converted dispatch positions, focus-class key
// routing, and the R-HUX-011 timestamp.

#include "context/editor/shell/input.h"

#include "shell_test.h"

#include <string>
#include <vector>

using namespace context::editor::shell;
namespace render = context::render;

namespace
{

PointerEvent pointer_at(PointerAction action, std::int32_t x, std::int32_t y,
                        MouseButton button = MouseButton::none)
{
    PointerEvent event;
    event.action = action;
    event.position = PointI{x, y};
    event.button = button;
    return event;
}

std::vector<ShellRegion> two_viewports()
{
    return {ShellRegion{"scene", shelltest::rect(0, 0, 400, 300), RegionKind::viewport},
            ShellRegion{"game", shelltest::rect(400, 0, 400, 300), RegionKind::viewport}};
}

void test_region_hit_test_is_back_to_front()
{
    RegionMap map;
    // Two OVERLAPPING regions: the later entry is stacked above, so it wins. This is the same rule
    // the UI package's hit_test uses, and it is what lets a publisher express stacking by order
    // alone rather than by a z field the two sides could disagree about.
    map.publish({ShellRegion{"under", shelltest::rect(0, 0, 200, 200), RegionKind::viewport},
                 ShellRegion{"over", shelltest::rect(50, 50, 100, 100), RegionKind::native}});

    const ShellRegion* hit = map.hit_test(PointI{100, 100});
    CHECK(hit != nullptr && hit->id == "over");

    const ShellRegion* under = map.hit_test(PointI{10, 10});
    CHECK(under != nullptr && under->id == "under");

    CHECK(map.hit_test(PointI{500, 500}) == nullptr);
}

void test_hit_test_edges_and_negative_coordinates()
{
    RegionMap map;
    map.publish({ShellRegion{"scene", shelltest::rect(10, 10, 100, 100), RegionKind::viewport}});

    CHECK(map.hit_test(PointI{10, 10}) != nullptr);   // inclusive origin
    CHECK(map.hit_test(PointI{109, 109}) != nullptr); // inclusive last texel
    CHECK(map.hit_test(PointI{110, 110}) == nullptr); // exclusive far edge
    CHECK(map.hit_test(PointI{9, 50}) == nullptr);

    // THE TRAP: a captured drag past the window's left/top edge reports a NEGATIVE coordinate. Cast
    // to unsigned it becomes an enormous positive number that lands inside almost any rect, silently
    // re-routing the drag. It must miss.
    CHECK(map.hit_test(PointI{-1, 50}) == nullptr);
    CHECK(map.hit_test(PointI{50, -1}) == nullptr);
    CHECK(map.hit_test(PointI{-5000, -5000}) == nullptr);
}

void test_publish_replaces_wholesale_and_bumps_generation()
{
    RegionMap map;
    CHECK(map.generation() == 0u);
    map.publish(two_viewports());
    CHECK(map.size() == 2u);
    CHECK(map.generation() == 1u);
    CHECK(map.find("scene") != nullptr);

    // A layout change publishes ONE consistent state; the old panel is GONE, not merged.
    map.publish({ShellRegion{"inspector", shelltest::rect(0, 0, 100, 100), RegionKind::native}});
    CHECK(map.size() == 1u);
    CHECK(map.find("scene") == nullptr);
    CHECK(map.generation() == 2u);
}

void test_pointer_arbitration_between_viewport_and_browser()
{
    InputArbiter arbiter;
    arbiter.regions().publish(two_viewports());

    const PointerDispatch in_scene = arbiter.route_pointer(pointer_at(PointerAction::move, 10, 20), 7);
    CHECK(in_scene.target == InputTarget::viewport);
    CHECK(in_scene.region_id == "scene");
    // Region-local physical coordinates for the native path.
    CHECK(in_scene.region_position == (PointI{10, 20}));
    // R-HUX-011: stamped at dispatch.
    CHECK(in_scene.dispatch_timestamp_us == 7u);

    const PointerDispatch in_game = arbiter.route_pointer(pointer_at(PointerAction::move, 450, 20), 8);
    CHECK(in_game.target == InputTarget::viewport);
    CHECK(in_game.region_id == "game");
    CHECK(in_game.region_position == (PointI{50, 20}));

    // Everywhere else is the browser's (§6.2).
    const PointerDispatch chrome = arbiter.route_pointer(pointer_at(PointerAction::move, 100, 500), 9);
    CHECK(chrome.target == InputTarget::browser);
    CHECK(chrome.region_id.empty());
}

void test_native_regions_route_to_the_native_target()
{
    InputArbiter arbiter;
    arbiter.regions().publish(
        {ShellRegion{"overlay", shelltest::rect(0, 0, 50, 50), RegionKind::native}});
    const PointerDispatch dispatch =
        arbiter.route_pointer(pointer_at(PointerAction::move, 10, 10), 1);
    // The kind is carried, not re-derived from an id naming convention.
    CHECK(dispatch.target == InputTarget::native);
    CHECK(dispatch.region_id == "overlay");
}

void test_browser_positions_are_dip_not_physical()
{
    InputArbiter arbiter;
    arbiter.set_dpi(DpiScale{192}); // 2x
    const PointerDispatch dispatch =
        arbiter.route_pointer(pointer_at(PointerAction::move, 200, 100), 1);
    CHECK(dispatch.target == InputTarget::browser);
    // A browser mouse event is in DIP; sending physical pixels would put the cursor at twice the
    // intended position on every non-100% monitor.
    CHECK(dispatch.logical_position == (PointI{100, 50}));
}

void test_implicit_drag_capture_keeps_a_drag_where_it_started()
{
    InputArbiter arbiter;
    arbiter.regions().publish(two_viewports());

    const PointerDispatch down =
        arbiter.route_pointer(pointer_at(PointerAction::down, 10, 20, MouseButton::left), 1);
    CHECK(down.target == InputTarget::viewport);
    CHECK(down.region_id == "scene");

    // The pointer crosses into the OTHER viewport mid-drag. Without the implicit capture this would
    // silently hand the gesture to "game" halfway through an orbit.
    const PointerDispatch drag = arbiter.route_pointer(pointer_at(PointerAction::move, 450, 20), 2);
    CHECK(drag.target == InputTarget::viewport);
    CHECK(drag.region_id == "scene");
    // Region-local coordinates go NEGATIVE-capable outside the region rather than clamping.
    CHECK(drag.region_position == (PointI{450, 20}));

    // Leaving the window entirely keeps the capture.
    const PointerDispatch outside = arbiter.route_pointer(pointer_at(PointerAction::move, -30, 20), 3);
    CHECK(outside.region_id == "scene");
    CHECK(outside.region_position.x == -30);

    // The release ends it.
    const PointerDispatch up =
        arbiter.route_pointer(pointer_at(PointerAction::up, 450, 20, MouseButton::left), 4);
    CHECK(up.region_id == "scene");
    CHECK(arbiter.capture_depth() == 0u);

    // Now plain arbitration is back.
    const PointerDispatch after = arbiter.route_pointer(pointer_at(PointerAction::move, 450, 20), 5);
    CHECK(after.region_id == "game");
}

void test_a_press_on_browser_chrome_captures_to_the_browser()
{
    InputArbiter arbiter;
    arbiter.regions().publish(two_viewports());
    // A press on chrome (outside every region) starts a browser-side drag; the pointer crossing a
    // viewport mid-drag must not hand the stream over — CEF is tracking its own drag.
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::down, 100, 500, MouseButton::left), 1)
              .target == InputTarget::browser);
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::move, 10, 20), 2).target ==
          InputTarget::browser);
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::up, 10, 20, MouseButton::left), 3).target ==
          InputTarget::browser);
    // ...and once released, the viewport gets it again.
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::move, 10, 20), 4).target ==
          InputTarget::viewport);
}

void test_a_second_button_does_not_retarget_a_live_drag()
{
    InputArbiter arbiter;
    arbiter.regions().publish(two_viewports());
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::down, 10, 20, MouseButton::left), 1)
              .region_id == "scene");
    // Right-pressing over the other viewport mid-drag: the capture is unchanged.
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::down, 450, 20, MouseButton::right), 2)
              .region_id == "scene");
    // And the RIGHT release does not end the LEFT drag.
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::up, 450, 20, MouseButton::right), 3)
              .region_id == "scene");
    CHECK(arbiter.capture_depth() == 0u); // the explicit stack stayed empty throughout
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::up, 450, 20, MouseButton::left), 4)
              .region_id == "scene");
    // Now released.
    CHECK(arbiter.route_pointer(pointer_at(PointerAction::move, 450, 20), 5).region_id == "game");
}

void test_modal_capture_swallows_a_miss_while_an_overlay_falls_through()
{
    InputArbiter arbiter;
    arbiter.regions().publish(
        {ShellRegion{"dropdown", shelltest::rect(100, 100, 80, 120), RegionKind::native},
         ShellRegion{"scene", shelltest::rect(0, 0, 400, 300), RegionKind::viewport}});

    // MODAL: the dropdown backdrop. A click outside it is eaten, not delivered to what is underneath.
    Capture modal;
    modal.region_id = "dropdown";
    modal.target = InputTarget::native;
    modal.modal = true;
    arbiter.push_capture(modal);

    CHECK(arbiter.route_pointer(pointer_at(PointerAction::move, 120, 140), 1).target ==
          InputTarget::native);
    const PointerDispatch outside = arbiter.route_pointer(pointer_at(PointerAction::move, 10, 10), 2);
    CHECK(outside.target == InputTarget::swallowed);
    CHECK(arbiter.swallowed() == 1);

    // Popping a capture that is not on top is refused — closing a popup blind is how the drag
    // capture pushed above it gets torn down by accident.
    CHECK(!arbiter.pop_capture("something-else"));
    CHECK(arbiter.pop_capture("dropdown"));
    CHECK(arbiter.capture_depth() == 0u);

    // OVERLAY (non-modal): a miss falls through to normal arbitration instead of being swallowed.
    Capture overlay;
    overlay.region_id = "dropdown";
    overlay.target = InputTarget::native;
    overlay.modal = false;
    arbiter.push_capture(overlay);
    const PointerDispatch fell_through =
        arbiter.route_pointer(pointer_at(PointerAction::move, 10, 10), 3);
    CHECK(fell_through.target == InputTarget::viewport);
    CHECK(fell_through.region_id == "scene");
}

void test_key_routing_follows_the_focus_class()
{
    InputArbiter arbiter;
    KeyEvent key;
    key.action = KeyAction::raw_key_down;
    key.windows_key_code = 'S';

    // No DOM editable focused and no keymap installed yet (e07): everything falls through to the
    // browser — the honest v1 behaviour.
    CHECK(arbiter.route_key(key, 11).target == InputTarget::browser);
    CHECK(arbiter.route_key(key, 11).dispatch_timestamp_us == 11u);

    // With a keymap that claims Ctrl+S, it goes to the command layer instead.
    arbiter.set_keymap_resolver(
        [](const KeyEvent& event, void*) { return event.modifiers.control && event.windows_key_code == 'S'; },
        nullptr);
    CHECK(arbiter.route_key(key, 12).target == InputTarget::browser); // no Ctrl
    key.modifiers.control = true;
    CHECK(arbiter.route_key(key, 13).target == InputTarget::keymap);

    // A DOM editable owns the keyboard OUTRIGHT — including accelerators. Swallowing a key the user
    // is typing into a text field is the failure this rule exists to prevent.
    arbiter.set_focus_class(FocusClass::dom_editable);
    CHECK(arbiter.route_key(key, 14).target == InputTarget::browser);

    // A CHAR event is never offered to the keymap: its RAWKEYDOWN already was, and offering both
    // would give one physical keystroke two chances to be claimed.
    arbiter.set_focus_class(FocusClass::none);
    KeyEvent character = key;
    character.action = KeyAction::character;
    CHECK(arbiter.route_key(character, 15).target == InputTarget::browser);
}

void test_dispatch_counters_make_a_round_trip_assertable()
{
    InputArbiter arbiter;
    arbiter.regions().publish(two_viewports());
    (void)arbiter.route_pointer(pointer_at(PointerAction::move, 10, 10), 1);
    (void)arbiter.route_pointer(pointer_at(PointerAction::move, 900, 900), 2);
    KeyEvent key;
    (void)arbiter.route_key(key, 3);
    CHECK(arbiter.pointer_dispatches() == 2);
    CHECK(arbiter.key_dispatches() == 1);
    CHECK(arbiter.swallowed() == 0);
}

} // namespace

int main()
{
    test_region_hit_test_is_back_to_front();
    test_hit_test_edges_and_negative_coordinates();
    test_publish_replaces_wholesale_and_bumps_generation();
    test_pointer_arbitration_between_viewport_and_browser();
    test_native_regions_route_to_the_native_target();
    test_browser_positions_are_dip_not_physical();
    test_implicit_drag_capture_keeps_a_drag_where_it_started();
    test_a_press_on_browser_chrome_captures_to_the_browser();
    test_a_second_button_does_not_retarget_a_live_drag();
    test_modal_capture_swallows_a_miss_while_an_overlay_falls_through();
    test_key_routing_follows_the_focus_class();
    test_dispatch_counters_make_a_round_trip_assertable();
    SHELL_TEST_MAIN_END();
}
