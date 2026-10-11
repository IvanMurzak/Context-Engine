// The macOS half of the smoke-tier real-mode injection arm (M9 e12c-3) — the Cocoa counterpart of
// smoke_window.cpp's X11 encoder. See smoke_window.h for the contract, the measured no-TCC-grant
// result, and the THREE Cocoa fidelity limits; see smoke_inject_cocoa.h for why this is a TU pair.
//
// WHAT MAKES THIS NON-VACUOUS, stated as the X11 arm states it. Nothing here posts into a queue the
// smoke owns. A synthesized NSEvent goes onto the APPLICATION's queue via
// `-[NSApplication postEvent:atStart:]` and comes back out of the very
// `-[NSApplication nextEventMatchingMask:]` call `CocoaWindowBackend::pump()` makes, is dispatched to
// its owner by `[event window]` exactly as a hardware event is, and is decoded by the SHIPPING
// `translate_ns_event`. So this arm cannot pass with the Cocoa pump broken, with the owner dispatch
// broken, with the decoder's pointer/key arms deleted, or against the headless backend (which
// `inject_event` refuses before reaching here). A `post()`-shaped seam on the real backend would have
// bypassed all four.
//
// FOUR COCOA SHAPES THIS FILE HAS TO GET RIGHT, each the inverse of one the backend applies:
//
//   1. THE COORDINATE INVERSE. A ShellEvent position is TOP-LEFT origin, PHYSICAL pixels. An NSEvent
//      carries `locationInWindow` — BOTTOM-LEFT origin, Cocoa POINTS, and relative to the WINDOW,
//      not the view. So: divide by the DPI factor, flip against the VIEW's height in points, then
//      let AppKit do the view->window step with -convertPoint:toView:nil rather than re-deriving the
//      titlebar offset by hand. Getting the flip wrong is invisible at the vertical centre and
//      mirrors everything else, which is why the live smoke asserts the flip DIRECTION.
//   2. THE MODIFIER INVERSE IS PARTIAL, BY CONSTRUCTION. The flags (shift/control/option/command)
//      travel on the event, so they are injectable. The pressed-BUTTON mask does not: the backend
//      reads it from `+[NSEvent pressedMouseButtons]`, a live HID query. An injected press therefore
//      arrives with `Modifiers::left_button_down` FALSE, and no amount of care here changes that —
//      documented in smoke_window.h so a smoke does not assert what cannot be delivered.
//   3. THE WINDOW IS FOUND, NOT PASSED. `IWindowBackend::native_window()` publishes the CAMetalLayer
//      (rhi.h's MetalLayer contract) and deliberately nothing else, so the window number an NSEvent
//      needs is recovered by scanning `[NSApp windows]` for the window whose contentView hosts that
//      exact layer. That keeps the SHIPPING window contract un-widened — issue #408's whole point —
//      and doubles as the anti-degrade check: a headless backend has no layer, so it cannot be found.
//   4. AN UNENCODABLE SHAPE IS REFUSED, NOT APPROXIMATED. There is no public NSEvent factory that can
//      set a scroll wheel's deltas or an OtherMouse event's button number, and macOS carries no
//      separate character event. Each of those refuses loudly, exactly as the X11 arm refuses its
//      own unencodable shapes — shipping an untested encoder is how a gate starts asserting fiction.

#include "smoke_inject_cocoa.h"

#if defined(__APPLE__)

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/smoke/smoke_window.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

namespace context::editor::shell::smoke
{
namespace
{

// The NSWindow whose contentView hosts `backend`'s CAMetalLayer, or nil.
//
// ⚠ THE `kind` CHECK IS LOAD-BEARING AND MUST COME FIRST. This TU is compiled -fobjc-arc, so the
// `__bridge` below produces a __strong local and ARC will `objc_retain` whatever pointer it is
// handed — dereferencing it. A backend reporting a non-Cocoa `kind` with a stand-in handle (the
// shape that SEGFAULTed render-present-test_present_blit on `build (macos-latest)`, CI run
// 30155542048) must therefore never reach the bridge. Only the real Cocoa backend publishes
// MetalLayer, and it publishes a real layer or nothing.
[[nodiscard]] NSWindow* cocoa_window_of(const IWindowBackend& backend)
{
    const render::NativeWindowDesc native = backend.native_window();
    if (native.kind != render::NativeWindowKind::MetalLayer || native.handle == nullptr)
    {
        return nil;
    }
    CALayer* layer = (__bridge CALayer*)native.handle;
    for (NSWindow* window in [NSApp windows])
    {
        NSView* view = [window contentView];
        if (view != nil && [view layer] == layer)
        {
            return window;
        }
    }
    return nil;
}

// The NSEvent modifierFlags mask for `modifiers` — the inverse of window.cpp's make_ns_modifiers over
// the four flags that are actually carried ON an event. The pressed-button half is deliberately
// absent (see file header, shape 2).
[[nodiscard]] NSEventModifierFlags ns_modifier_mask(const Modifiers& modifiers)
{
    NSEventModifierFlags mask = 0;
    if (modifiers.shift)
    {
        mask |= static_cast<NSEventModifierFlags>(kNsModifierShift);
    }
    if (modifiers.control)
    {
        mask |= static_cast<NSEventModifierFlags>(kNsModifierControl);
    }
    // Option IS Alt and Command IS Meta, the same pairing make_ns_modifiers documents — mapping
    // `meta` onto Control here would inject a Ctrl-shortcut for every Cmd-shortcut a smoke drives.
    if (modifiers.alt)
    {
        mask |= static_cast<NSEventModifierFlags>(kNsModifierOption);
    }
    if (modifiers.meta)
    {
        mask |= static_cast<NSEventModifierFlags>(kNsModifierCommand);
    }
    return mask;
}

// The window-space Cocoa point for a Shell-space physical position — the exact inverse of
// ns_view_point_to_physical, with the view->window step left to AppKit (file header, shape 1).
//
// The ARITHMETIC is `ns_view_point_for_physical` in the portable seam TU, where every leg compiles
// it and a round-trip sweep pins it against the shipping decoder (see its header comment for why
// that matters: the live smoke's flip/separation/half-plane claims cannot catch a wrong SCALE).
// Only the two AppKit reads it needs are here.
//
// REFUSES rather than approximating when the window carries no contentView. Returning a fabricated
// location would make the smoke fail later on a wrong delivered POSITION instead of here on the
// honest "there is no view to inject into" — and every other guard in this file refuses (file
// header, shape 4). Unreachable today (`cocoa_window.mm` always sets a contentView), so this is
// defence in depth, not a live path.
[[nodiscard]] bool window_point_for(NSWindow* window, const IWindowBackend& backend,
                                    PointI position, NSPoint* out_location)
{
    NSView* view = [window contentView];
    if (view == nil || out_location == nullptr)
    {
        return false;
    }
    const NsViewPointPoints in_points = ns_view_point_for_physical(
        position, static_cast<double>([view bounds].size.height), backend.dpi());
    const NSPoint in_view = NSMakePoint(static_cast<CGFloat>(in_points.x_points),
                                        static_cast<CGFloat>(in_points.y_points));
    *out_location = [view convertPoint:in_view toView:nil];
    return true;
}

[[nodiscard]] bool inject_pointer_cocoa(NSWindow* window, const IWindowBackend& backend,
                                        const PointerEvent& pointer)
{
    NSEventType type = NSEventTypeMouseMoved;
    switch (pointer.action)
    {
    case PointerAction::move:
        type = NSEventTypeMouseMoved;
        break;
    case PointerAction::down:
    case PointerAction::up:
        switch (pointer.button)
        {
        case MouseButton::left:
            type = pointer.action == PointerAction::down ? NSEventTypeLeftMouseDown
                                                        : NSEventTypeLeftMouseUp;
            break;
        case MouseButton::right:
            type = pointer.action == PointerAction::down ? NSEventTypeRightMouseDown
                                                         : NSEventTypeRightMouseUp;
            break;
        case MouseButton::middle:
            // NSEventTypeOtherMouseDown is what the middle button arrives as, and the decoder tells
            // it apart by -buttonNumber (kNsOtherButtonMiddle) — which
            // +mouseEventWithType:...:pressure: cannot set. Injecting one anyway would deliver an
            // OtherMouse event with buttonNumber 0, which the decoder correctly reads as NOT the
            // middle button, so the smoke would see a sample it never asked for. Refused instead.
            return false;
        case MouseButton::none:
            return false; // a press/release with no button is not an event Cocoa can carry
        }
        break;
    case PointerAction::wheel:
    case PointerAction::leave:
        // A wheel notch needs -scrollingDeltaY and a leave needs the tracking-area machinery; NO
        // public NSEvent factory sets either, so there is nothing honest to synthesize. No smoke
        // injects them today (the X11 arm refuses both for its own reasons), so this refuses loudly
        // rather than shipping an encoder no test could exercise.
        return false;
    }

    NSPoint location = NSZeroPoint;
    if (!window_point_for(window, backend, pointer.position, &location))
    {
        return false;
    }
    NSEvent* event = [NSEvent mouseEventWithType:type
                                       location:location
                                  modifierFlags:ns_modifier_mask(pointer.modifiers)
                                      timestamp:[[NSProcessInfo processInfo] systemUptime]
                                   windowNumber:[window windowNumber]
                                        context:nil
                                    eventNumber:0
                                     clickCount:pointer.click_count > 0 ? pointer.click_count : 1
                                       pressure:type == NSEventTypeLeftMouseDown ||
                                                        type == NSEventTypeRightMouseDown
                                                    ? 1.0f
                                                    : 0.0f];
    if (event == nil)
    {
        return false;
    }
    // atStart:NO — appended, so a move/press/release triple arrives in the order it was injected.
    // Prepending would deliver a release before its press and the decoder's arbitration would see a
    // sequence no hardware produces.
    [NSApp postEvent:event atStart:NO];
    return true;
}

[[nodiscard]] bool inject_key_cocoa(NSWindow* window, const KeyEvent& key)
{
    NSEventType type = NSEventTypeKeyDown;
    switch (key.action)
    {
    case KeyAction::raw_key_down:
    case KeyAction::key_down:
        type = NSEventTypeKeyDown;
        break;
    case KeyAction::key_up:
        type = NSEventTypeKeyUp;
        break;
    case KeyAction::character:
        // macOS carries no separate character event either: the character rides ON the key event and
        // translate_ns_event forwards it from there. Injecting one directly would be inventing a
        // native event shape that does not exist — the same refusal, for the same reason, that the
        // X11 arm records.
        return false;
    }

    const NsVirtualKey virtual_key = ns_virtual_key_for_windows_key_code(key.windows_key_code);
    if (!virtual_key.covered)
    {
        return false;
    }
    NSString* characters = @"";
    if (virtual_key.text != 0)
    {
        const unichar unit = static_cast<unichar>(virtual_key.text);
        characters = [NSString stringWithCharacters:&unit length:1];
    }
    NSEvent* event = [NSEvent keyEventWithType:type
                                     location:NSZeroPoint
                                modifierFlags:ns_modifier_mask(key.modifiers)
                                    timestamp:[[NSProcessInfo processInfo] systemUptime]
                                 windowNumber:[window windowNumber]
                                      context:nil
                                   characters:characters
                  charactersIgnoringModifiers:characters
                                    isARepeat:NO
                                      keyCode:static_cast<unsigned short>(virtual_key.key_code)];
    if (event == nil)
    {
        return false;
    }
    [NSApp postEvent:event atStart:NO];
    return true;
}

} // namespace

bool inject_cocoa_event(const IWindowBackend& backend, const ShellEvent& event)
{
    @autoreleasepool
    {
        // NSApp is nil until [NSApplication sharedApplication] has run. It always has by here — the
        // Cocoa backend brings it up in create() before it can publish a layer — but a nil NSApp
        // would silently swallow the postEvent: (Objective-C's nil-receiver amnesty), so the smoke
        // would wait for an event that was never queued and time out with no reason given.
        if (NSApp == nil)
        {
            return false;
        }
        NSWindow* window = cocoa_window_of(backend);
        if (window == nil)
        {
            return false;
        }
        switch (event.kind)
        {
        case ShellEventKind::pointer:
            return inject_pointer_cocoa(window, backend, event.pointer);
        case ShellEventKind::key:
            return inject_key_cocoa(window, event.key);
        default:
            return false;
        }
    }
}

} // namespace context::editor::shell::smoke

#endif // __APPLE__
