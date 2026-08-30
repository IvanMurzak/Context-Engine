// The macOS Cocoa window backend (design 03 §1) — NSWindow + a layer-backed NSView, the integrated
// NSEvent pump, backingScaleFactor DPI, and Cocoa-space placement.
//
// This file is the OS-call half of the macOS seam, the exact counterpart of win32_window.cpp and
// x11_window.cpp. All of the event DECODING lives in window.cpp as pure functions compiled and
// tested on every OS — see window.h for why. What remains here is genuinely AppKit-only and honestly
// untested off macOS: NO local gate compiles an `__APPLE__` branch at all, and until M9 e12c-3 no
// CI job RAN a windowed macOS test (the `build (macos-latest)` leg compiles and links this file and
// executes the pure decoders; it opens no window unless the runner has a GUI session — the windowed
// proof is `editor-shell-cocoa-window`, in the `editor-cef-smoke (macos-latest)` job).
//
// CEF-FREE BY CONSTRUCTION (e12b's scope). A CEF browser on macOS needs an `.app` bundle, FIVE
// per-process-type helper bundles and the runtime-loaded framework; that packaging LANDED in e12c-1
// (issue #436) for `context_editor` + two smokes and in e12c-2 for the remaining seven — see
// src/editor/shell/CMakeLists.txt's `context_shell_cef_mac_bundle()`. Nothing here includes a CEF
// header, exactly as nothing in `context_editor_shell` does on the other two platforms.
//
// FIVE COCOA SHAPES THAT DIFFER FROM WIN32/X11 AND ARE EASY TO GET WRONG:
//
//   1. THE Y AXIS POINTS UP, AND COORDINATES ARE POINTS. Every position crosses into the Shell's
//      top-left PHYSICAL-PIXEL space through ns_view_point_to_physical, which is pure and tested.
//      A missing flip mirrors the UI vertically; a missing scale is invisible at 1x and halves every
//      coordinate on a Retina display.
//   2. THERE IS NO WINDOW-EVENT QUEUE. Resize / move / backing-scale changes are delegate callbacks,
//      not NSEvents — so geometry is POLLED once per pump and diffed by translate_ns_window_geometry,
//      which is the Cocoa counterpart of the X11 ConfigureNotify comparison.
//   3. ONE APPLICATION, N WINDOWS, ONE QUEUE. `-[NSApplication nextEventMatchingMask:]` drains the
//      whole process queue, so — exactly like the Win32 pump's NULL hwnd filter and the X11 pump's
//      single Display queue — events are dispatched to their owner through a process-wide registry.
//   4. EVERY DEQUEUED EVENT MUST STILL REACH APPKIT. The Shell OBSERVES the stream; swallowing it
//      breaks window dragging, the menu, the IME and the standard key equivalents.
//   5. ARC + NSWindow NEEDS `setReleasedWhenClosed:NO`. A window created with -initWithContentRect:
//      releases itself on close, which under ARC is one release too many and crashes on teardown.

#include "context/editor/shell/window.h"

#if defined(__APPLE__)

#include "context/editor/shell/cocoa_chrome.h"
#include "context/editor/shell/cocoa_menu.h"
#include "context/editor/shell/menu_model.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

// The locally-declared NSEvent values the pure decoder uses, asserted against the REAL ones here,
// where <AppKit/AppKit.h> is available. A wrong constant is therefore a macOS COMPILE error rather
// than an event that silently decodes as something else at runtime on the one platform that runs it.
//
// The macOS VIRTUAL KEY CODES are deliberately NOT asserted this way: they live in
// <Carbon/HIToolbox/Events.h>, and importing Carbon into an AppKit-only translation unit is a
// heavier dependency than the assertion is worth for values that have been frozen since the ADB
// keyboard. They are pinned instead by the table test in editor-shell-test_window, which runs on all
// three legs.
using context::editor::shell::kNsFlagsChanged;
using context::editor::shell::kNsKeyDown;
using context::editor::shell::kNsKeyUp;
using context::editor::shell::kNsLeftMouseDown;
using context::editor::shell::kNsLeftMouseDragged;
using context::editor::shell::kNsLeftMouseUp;
using context::editor::shell::kNsMouseEntered;
using context::editor::shell::kNsMouseExited;
using context::editor::shell::kNsMouseMoved;
using context::editor::shell::kNsOtherMouseDown;
using context::editor::shell::kNsOtherMouseDragged;
using context::editor::shell::kNsOtherMouseUp;
using context::editor::shell::kNsRightMouseDown;
using context::editor::shell::kNsRightMouseDragged;
using context::editor::shell::kNsRightMouseUp;
using context::editor::shell::kNsScrollWheel;

static_assert(kNsLeftMouseDown == static_cast<std::int32_t>(NSEventTypeLeftMouseDown),
              "NSEventTypeLeftMouseDown drifted");
static_assert(kNsLeftMouseUp == static_cast<std::int32_t>(NSEventTypeLeftMouseUp),
              "NSEventTypeLeftMouseUp drifted");
static_assert(kNsRightMouseDown == static_cast<std::int32_t>(NSEventTypeRightMouseDown),
              "NSEventTypeRightMouseDown drifted");
static_assert(kNsRightMouseUp == static_cast<std::int32_t>(NSEventTypeRightMouseUp),
              "NSEventTypeRightMouseUp drifted");
static_assert(kNsMouseMoved == static_cast<std::int32_t>(NSEventTypeMouseMoved),
              "NSEventTypeMouseMoved drifted");
static_assert(kNsLeftMouseDragged == static_cast<std::int32_t>(NSEventTypeLeftMouseDragged),
              "NSEventTypeLeftMouseDragged drifted");
static_assert(kNsRightMouseDragged == static_cast<std::int32_t>(NSEventTypeRightMouseDragged),
              "NSEventTypeRightMouseDragged drifted");
static_assert(kNsMouseEntered == static_cast<std::int32_t>(NSEventTypeMouseEntered),
              "NSEventTypeMouseEntered drifted");
static_assert(kNsMouseExited == static_cast<std::int32_t>(NSEventTypeMouseExited),
              "NSEventTypeMouseExited drifted");
static_assert(kNsKeyDown == static_cast<std::int32_t>(NSEventTypeKeyDown),
              "NSEventTypeKeyDown drifted");
static_assert(kNsKeyUp == static_cast<std::int32_t>(NSEventTypeKeyUp), "NSEventTypeKeyUp drifted");
static_assert(kNsFlagsChanged == static_cast<std::int32_t>(NSEventTypeFlagsChanged),
              "NSEventTypeFlagsChanged drifted");
static_assert(kNsScrollWheel == static_cast<std::int32_t>(NSEventTypeScrollWheel),
              "NSEventTypeScrollWheel drifted");
static_assert(kNsOtherMouseDown == static_cast<std::int32_t>(NSEventTypeOtherMouseDown),
              "NSEventTypeOtherMouseDown drifted");
static_assert(kNsOtherMouseUp == static_cast<std::int32_t>(NSEventTypeOtherMouseUp),
              "NSEventTypeOtherMouseUp drifted");
static_assert(kNsOtherMouseDragged == static_cast<std::int32_t>(NSEventTypeOtherMouseDragged),
              "NSEventTypeOtherMouseDragged drifted");

static_assert(context::editor::shell::kNsModifierCapsLock ==
                  static_cast<std::uint64_t>(NSEventModifierFlagCapsLock),
              "NSEventModifierFlagCapsLock drifted");
static_assert(context::editor::shell::kNsModifierShift ==
                  static_cast<std::uint64_t>(NSEventModifierFlagShift),
              "NSEventModifierFlagShift drifted");
static_assert(context::editor::shell::kNsModifierControl ==
                  static_cast<std::uint64_t>(NSEventModifierFlagControl),
              "NSEventModifierFlagControl drifted");
static_assert(context::editor::shell::kNsModifierOption ==
                  static_cast<std::uint64_t>(NSEventModifierFlagOption),
              "NSEventModifierFlagOption drifted");
static_assert(context::editor::shell::kNsModifierCommand ==
                  static_cast<std::uint64_t>(NSEventModifierFlagCommand),
              "NSEventModifierFlagCommand drifted");
static_assert(context::editor::shell::kNsModifierNumericPad ==
                  static_cast<std::uint64_t>(NSEventModifierFlagNumericPad),
              "NSEventModifierFlagNumericPad drifted");
static_assert(context::editor::shell::kNsModifierFunction ==
                  static_cast<std::uint64_t>(NSEventModifierFlagFunction),
              "NSEventModifierFlagFunction drifted");

} // namespace

// The mailbox the window delegate writes into. At FILE scope, outside every namespace, because an
// Objective-C @interface cannot be declared inside a C++ namespace — and keeping the delegate's
// entire knowledge of the Shell down to three bools is what stops the Objective-C/C++ boundary from
// growing into a second copy of the backend's state.
struct ContextShellWindowMailbox
{
    bool close_requested = false;
    bool focus_gained = false;
    bool focus_lost = false;
};

@interface ContextShellWindowDelegate : NSObject <NSWindowDelegate>
{
@public
    ContextShellWindowMailbox* mailbox;
}
@end

@implementation ContextShellWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    (void)sender;
    if (mailbox != nullptr)
    {
        mailbox->close_requested = true;
    }
    // NO — deliberately. The owner loop observes the close_requested event and tears the window down
    // in ITS order, which is the exact counterpart of the Win32 backend posting itself a WM_CLOSE and
    // the X11 backend sending itself a WM_DELETE_WINDOW. Returning YES would let AppKit destroy the
    // window underneath a compositor that is mid-frame.
    return NO;
}

- (void)windowDidBecomeKey:(NSNotification*)notification
{
    (void)notification;
    if (mailbox != nullptr)
    {
        mailbox->focus_gained = true;
    }
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    (void)notification;
    if (mailbox != nullptr)
    {
        mailbox->focus_lost = true;
    }
}

@end

// The ONE target every d3 NSMenuItem points at (cocoa_menu.h). At FILE scope for the same reason the
// delegate is; its entire knowledge of the Shell is ONE dispatch function pointer, aimed at a member
// the owning backend keeps alive for its whole life (the mailbox discipline). The command id rides
// the item's own representedObject, so this class holds no per-item state at all.
@interface ContextShellMenuTarget : NSObject
{
@public
    std::function<void(const std::string&)>* dispatch;
}
- (void)contextShellMenuActivate:(NSMenuItem*)sender;
@end

@implementation ContextShellMenuTarget

- (void)contextShellMenuActivate:(NSMenuItem*)sender
{
    id represented = [sender representedObject];
    if (dispatch == nullptr || !(*dispatch) || ![represented isKindOfClass:[NSString class]])
    {
        return; // an unwired or nameless item dispatches nothing — never a crash, never a guess
    }
    const char* utf8 = [(NSString*)represented UTF8String];
    if (utf8 != nullptr)
    {
        (*dispatch)(std::string(utf8));
    }
}

@end

namespace context::editor::shell
{
namespace
{

class CocoaWindowBackend;

// One queue serves every window in the process, exactly like the Win32 pump's NULL hwnd filter and
// the X11 pump's single Display queue — so whichever backend pumps drains its siblings' events too
// and dispatches them by owner.
std::vector<CocoaWindowBackend*>& cocoa_windows()
{
    static std::vector<CocoaWindowBackend*> windows;
    return windows;
}

// Round-half-away-from-zero, NaN/inf safe — a verbatim twin of window.cpp's file-local
// ns_round_to_int. `static_cast<std::int32_t>` of an out-of-range double is UB, and a Cocoa geometry
// read during teardown can legitimately be infinite.
//
// WHY A COPY IS RIGHT HERE, stated precisely, because the obvious reason is wrong: window.h DOES
// export pure arithmetic (ns_dpi_from_backing_scale, ns_extent_to_physical, ns_view_point_to_physical),
// so "no arithmetic in the header" is not the rule. The rule is that the header publishes named SEAM
// CONCEPTS — a Cocoa point extent becoming physical pixels — and not generic numeric primitives.
// Rounding a double is the latter. The distinction is load-bearing rather than cosmetic: every
// remaining caller below feeds a value that is only ever compared against other cocoa_round_i32
// outputs (frame origins in, frame origins out, round-tripping through apply_placement), so this copy
// is self-consistent by construction. The moment a value crosses into something the pure decoders
// also compute — as the initial client size did — it must use the EXPORTED function instead, which is
// exactly why create() no longer rounds inline.
[[nodiscard]] std::int32_t cocoa_round_i32(double value)
{
    if (!std::isfinite(value))
    {
        return 0;
    }
    const double clamped = std::min(std::max(value, -1.0e9), 1.0e9);
    return static_cast<std::int32_t>(clamped < 0.0 ? -std::floor(-clamped + 0.5)
                                                   : std::floor(clamped + 0.5));
}

// Is there a GUI (Aqua) session to open a window in? `CGSessionCopyCurrentDictionary` answers NULL
// from a process with no window-server connection — a build bot under plain launchd or ssh, which is
// the ordinary state of a headless CI leg and NOT a crash. Probed BEFORE
// `[NSApplication sharedApplication]`, because it is that call which aborts the process outright
// ("_RegisterApplication(), FAILED TO establish the default connection to the WindowServer") when
// there is none. This is the macOS counterpart of the X11 backend reporting "no reachable X server".
[[nodiscard]] bool cocoa_gui_session_available()
{
    CFDictionaryRef session = ::CGSessionCopyCurrentDictionary();
    if (session == nullptr)
    {
        return false;
    }
    CFRelease(session);
    return true;
}

// Bring up NSApplication once per process. `finishLaunching` is what makes AppKit deliver events at
// all to an app that was not started by LaunchServices.
void cocoa_ensure_application()
{
    static const bool started = [] {
        @autoreleasepool
        {
            [NSApplication sharedApplication];
            (void)[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp finishLaunching];
        }
        return true;
    }();
    (void)started;
}

[[nodiscard]] NSString* cocoa_string(std::string_view utf8)
{
    NSString* value = [[NSString alloc] initWithBytes:utf8.data()
                                               length:utf8.size()
                                             encoding:NSUTF8StringEncoding];
    return value != nil ? value : @"";
}

// --------------------------------------------------------------------------- the d3 menu builders
// (cocoa_menu.h). Pure-ish AppKit assembly: everything DECIDED here — what parses, what an
// accelerator means — was decided by menu_model.h's tested functions; these only transcribe.

// One parsed accelerator becoming an NSMenuItem key equivalent. `Ctrl` (and `Meta`) map onto the
// COMMAND key — the platform's primary modifier, the CmdOrCtrl reading cocoa_menu.h pins — so the
// published `Ctrl+Z` accelerator is the ⌘Z macOS users expect. A key token this table cannot name
// yields the honest "no equivalent" (empty string, zero mask) rather than a guessed binding.
[[nodiscard]] NSString* ns_key_equivalent(const context::editor::shell::MenuAccelerator& accel,
                                          NSEventModifierFlags& mask)
{
    mask = 0;
    if (accel.ctrl || accel.meta)
    {
        mask |= NSEventModifierFlagCommand;
    }
    if (accel.shift)
    {
        mask |= NSEventModifierFlagShift;
    }
    if (accel.alt)
    {
        mask |= NSEventModifierFlagOption;
    }
    if (accel.key.size() == 1)
    {
        return cocoa_string(accel.key);
    }
    unichar function_key = 0;
    if (accel.key == "ArrowLeft")
    {
        function_key = NSLeftArrowFunctionKey;
    }
    else if (accel.key == "ArrowRight")
    {
        function_key = NSRightArrowFunctionKey;
    }
    else if (accel.key == "ArrowUp")
    {
        function_key = NSUpArrowFunctionKey;
    }
    else if (accel.key == "ArrowDown")
    {
        function_key = NSDownArrowFunctionKey;
    }
    if (function_key == 0)
    {
        mask = 0;
        return @"";
    }
    return [NSString stringWithCharacters:&function_key length:1];
}

// Build one NSMenu from parsed items, recursively for submenus, counting the COMMAND items kept
// (the CocoaMenuStats::items observable). `autoenablesItems` is OFF on every menu built here:
// enablement is the PUBLISHED model's snapshot (cocoa_menu.h), never AppKit's responder-chain guess.
[[nodiscard]] NSMenu* build_ns_menu(const std::string& title,
                                    const std::vector<context::editor::shell::MenuItem>& items,
                                    ContextShellMenuTarget* target, std::size_t& command_items)
{
    using context::editor::shell::MenuItem;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:cocoa_string(title)];
    [menu setAutoenablesItems:NO];
    for (const MenuItem& item : items)
    {
        switch (item.kind)
        {
        case MenuItem::Kind::separator:
            [menu addItem:[NSMenuItem separatorItem]];
            break;
        case MenuItem::Kind::submenu:
        {
            NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:cocoa_string(item.label)
                                                            action:nil
                                                     keyEquivalent:@""];
            [holder setSubmenu:build_ns_menu(item.label, item.items, target, command_items)];
            [menu addItem:holder];
            break;
        }
        case MenuItem::Kind::command:
        {
            NSString* key = @"";
            NSEventModifierFlags mask = 0;
            const std::optional<context::editor::shell::MenuAccelerator> accel =
                context::editor::shell::parse_menu_accelerator(item.accelerator);
            if (accel.has_value())
            {
                key = ns_key_equivalent(*accel, mask);
            }
            NSMenuItem* entry =
                [[NSMenuItem alloc] initWithTitle:cocoa_string(item.label)
                                           action:@selector(contextShellMenuActivate:)
                                    keyEquivalent:key];
            [entry setKeyEquivalentModifierMask:mask];
            [entry setTarget:target];
            [entry setEnabled:(item.enabled ? YES : NO)];
            // The command id rides the item itself, so the shared target stays stateless.
            [entry setRepresentedObject:cocoa_string(item.command_id)];
            if (!item.tooltip.empty())
            {
                [entry setToolTip:cocoa_string(item.tooltip)];
            }
            [menu addItem:entry];
            ++command_items;
            break;
        }
        }
    }
    return menu;
}

// Find the command item carrying `command_id`, depth-first — the programmatic-activation lookup
// (cocoa_menu.h `cocoa_menu_perform`).
[[nodiscard]] NSMenuItem* find_ns_menu_item(NSMenu* menu, NSString* command_id)
{
    for (NSMenuItem* item in [menu itemArray])
    {
        if ([item hasSubmenu])
        {
            NSMenuItem* found = find_ns_menu_item([item submenu], command_id);
            if (found != nil)
            {
                return found;
            }
            continue;
        }
        id represented = [item representedObject];
        if ([represented isKindOfClass:[NSString class]] &&
            [(NSString*)represented isEqualToString:command_id])
        {
            return item;
        }
    }
    return nil;
}

// ------------------------------------------------------------------------------------ the backend

class CocoaWindowBackend final : public IWindowBackend
{
public:
    CocoaWindowBackend() = default;

    CocoaWindowBackend(const CocoaWindowBackend&) = delete;
    CocoaWindowBackend& operator=(const CocoaWindowBackend&) = delete;

    ~CocoaWindowBackend() override { destroy(); }

    [[nodiscard]] bool create(const WindowDesc& desc, std::string& error);

    [[nodiscard]] const char* name() const override { return "cocoa"; }

    [[nodiscard]] render::NativeWindowDesc native_window() const override
    {
        render::NativeWindowDesc native;
        if (layer_ == nil)
        {
            return native; // kind stays None — the honest "nothing presentable here"
        }
        // rhi.h's MetalLayer contract: the handle is the CAMetalLayer backing the NSView. It is the
        // layer and NOT the view, because both the GPU surface (wgpuInstanceCreateSurface) and the
        // CPU present fallback (CALayer.contents) address the layer.
        native.kind = render::NativeWindowKind::MetalLayer;
        native.handle = (__bridge void*)layer_;
        // No second handle: unlike X11's (Display*, Window) pair, a CAMetalLayer is self-contained.
        return native;
    }

    [[nodiscard]] render::Extent2D client_size() const override { return size_; }
    [[nodiscard]] DpiScale dpi() const override { return geometry_.dpi; }
    [[nodiscard]] bool alive() const override { return window_ != nil && alive_; }

    bool pump(std::vector<ShellEvent>& out) override;

    void request_redraw() override
    {
        if (view_ != nil)
        {
            [view_ setNeedsDisplay:YES];
        }
    }

    void request_activation() override
    {
        // Best-effort single-instance FOCUS (M9 e14b, D15/C-F23), the macOS counterpart of the Win32
        // and X11 overrides. Deliberately does NOT call -activateIgnoringOtherApps: — it is
        // deprecated since macOS 14, and a deprecation warning is an ERROR under this repo's -Werror
        // baseline on the one leg that compiles this file. macOS refuses cross-application focus
        // steals anyway, exactly as SetForegroundWindow and _NET_ACTIVE_WINDOW may, so
        // deminiaturize + makeKeyAndOrderFront is the whole honest ask.
        if (window_ == nil)
        {
            return;
        }
        if ([window_ isMiniaturized])
        {
            [window_ deminiaturize:nil];
        }
        [window_ makeKeyAndOrderFront:nil];
    }

    void set_title(std::string_view title) override
    {
        if (window_ != nil)
        {
            @autoreleasepool
            {
                [window_ setTitle:cocoa_string(title)];
            }
        }
    }

    // a1 (editor-window-chrome): the two chrome verbs, in the platform's own vocabulary —
    // `miniaturize:` (the Dock genie) and `zoom:` (the platform's maximize). `zoom:` is a TOGGLE,
    // so the bool target is expressed by guarding on `isZoomed` — the same ordering-safe shape
    // apply_placement already uses. Best-effort asks, like request_activation.
    void minimize() override
    {
        if (window_ != nil && ![window_ isMiniaturized])
        {
            [window_ miniaturize:nil];
        }
    }
    void set_maximized(bool maximized) override
    {
        if (window_ == nil)
        {
            return;
        }
        if (maximized != ([window_ isZoomed] != NO))
        {
            [window_ zoom:nil];
        }
    }

    [[nodiscard]] WindowPlacement placement() const override;
    void apply_placement(const WindowPlacement& placement) override;

    void close() override
    {
        // Records the request rather than closing: the owner loop observes close_requested on the
        // next pump and shuts down in its own order. Identical in shape to the Win32 backend's
        // PostMessage(WM_CLOSE) and the X11 backend's self-addressed WM_DELETE_WINDOW.
        mailbox_.close_requested = true;
    }

    // Called by whichever backend is pumping, for an event addressed to THIS window. Returns TRUE
    // when the event was CONSUMED by the c1 caption consult (handed to performWindowDragWithEvent:
    // or zoom:), in which case the pump must NOT forward it to [NSApp sendEvent:] — that hand-off
    // IS its AppKit consumption — and no ShellEvent was queued for it.
    [[nodiscard]] bool handle(NSEvent* event);
    [[nodiscard]] NSWindow* window() const { return window_; }

    // --- the c1 hybrid-chrome surface (cocoa_chrome.h; reached via the name()-keyed free
    // --- functions at the bottom of this file) ---------------------------------------------------
    void bind_caption_arbiter(const InputArbiter* arbiter) { caption_arbiter_ = arbiter; }
    [[nodiscard]] CocoaCaptionStats caption_stats() const
    {
        return CocoaCaptionStats{caption_drags_, caption_double_clicks_, caption_yields_};
    }
    [[nodiscard]] bool hybrid_chrome(CocoaChromeState& out) const;

    // --- the d3 native-menu surface (cocoa_menu.h; reached the same name()-keyed way) ------------
    [[nodiscard]] bool install_menu(const MenuModel& model, MenuActivationCallback on_activate);
    [[nodiscard]] CocoaMenuStats menu_stats() const
    {
        return CocoaMenuStats{menu_installs_, menu_items_, menu_activations_};
    }
    [[nodiscard]] bool perform_menu(const std::string& command_id);

private:
    void destroy();
    void drain_geometry();
    void drain_mailbox();
    [[nodiscard]] NsWindowGeometry read_geometry() const;

    NSWindow* window_ = nil;
    NSView* view_ = nil;
    CAMetalLayer* layer_ = nil;
    ContextShellWindowDelegate* delegate_ = nil;
    ContextShellWindowMailbox mailbox_;
    std::vector<ShellEvent> pending_;
    NsWindowGeometry geometry_;
    render::Extent2D size_{};
    // The modifier mask as of the PREVIOUS event, which is the only way a FlagsChanged can be read
    // as a press or a release (see translate_ns_event).
    std::uint64_t modifier_flags_ = 0;
    // c1: the window's LIVE input arbiter (cocoa_bind_caption_arbiter), so the pump can consult the
    // published `caption` rects AND the live capture state while the NSEvent is still in hand. Read
    // ONLY inside handle(), which only runs from pump(), so the EditorWindow member-destruction
    // order (its arbiter dies before this backend) can never race a read.
    const InputArbiter* caption_arbiter_ = nullptr;
    std::size_t caption_drags_ = 0;
    std::size_t caption_double_clicks_ = 0;
    std::size_t caption_yields_ = 0;
    // d3: the installed global menu bar + its one shared target. `menu_dispatch_` is the member the
    // target aims at (the mailbox discipline: the ObjC object knows one pointer, the backend owns
    // the lifetime) — it wraps the caller's callback with the activation counter, so the two can
    // never disagree about how many activations happened.
    NSMenu* main_menu_ = nil;
    ContextShellMenuTarget* menu_target_ = nil;
    std::function<void(const std::string&)> menu_dispatch_;
    std::size_t menu_installs_ = 0;
    std::size_t menu_items_ = 0;
    std::size_t menu_activations_ = 0;
    bool alive_ = true;
};

bool CocoaWindowBackend::create(const WindowDesc& desc, std::string& error)
{
    if (!cocoa_gui_session_available())
    {
        error = "no GUI (Aqua) session: macOS window creation needs a WindowServer connection, and "
                "this process has none (a build bot under launchd or ssh is the ordinary case)";
        return false;
    }
    cocoa_ensure_application();

    @autoreleasepool
    {
        // ⚠ COCOA GEOMETRY IS IN POINTS, NOT PIXELS — the opposite of Win32 and X11, where the OS
        // speaks physical pixels and the backend derives the physical size from the DPI itself. Here
        // AppKit owns the backing store, so the window is created at the LOGICAL size and the
        // backing scale is read back afterwards. A to_physical() here would double the window on
        // every Retina display.
        NSRect content = NSMakeRect(0, 0, static_cast<CGFloat>(desc.logical_size.width),
                                    static_cast<CGFloat>(desc.logical_size.height));
        if (desc.placement.has_value() && !render::is_empty(desc.placement->size()))
        {
            // See placement() for why a persisted macOS placement is in POINTS: it is written and
            // read by this backend alone and round-trips exactly.
            content = NSMakeRect(static_cast<CGFloat>(desc.placement->x),
                                 static_cast<CGFloat>(desc.placement->y),
                                 static_cast<CGFloat>(desc.placement->width),
                                 static_cast<CGFloat>(desc.placement->height));
        }

        // HYBRID CHROME (editor-window-chrome c1, target design 02 §4): the content view spans the
        // FULL window and the titlebar draws transparent over it, so the a2 web titlebar strip is
        // the visible top of the window while the native TRAFFIC LIGHTS stay exactly where macOS
        // puts them, floating above the strip's measured inset (cocoa_chrome.h). Titled stays in
        // the mask — it is what makes the standard buttons exist at all — and the title string is
        // still SET below (Mission Control, the Dock and the window menu read it); only the
        // in-titlebar rendering is hidden, because the strip renders the title itself.
        const NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                                 NSWindowStyleMaskFullSizeContentView;
        window_ = [[NSWindow alloc] initWithContentRect:content
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        if (window_ == nil)
        {
            error = "[NSWindow initWithContentRect:] returned nil";
            return false;
        }
        // ⚠ ARC. An NSWindow created this way defaults to releasing ITSELF when closed, which under
        // ARC — where the strong reference above is also released — is one release too many and
        // crashes on teardown. See the file header, shape 5.
        [window_ setReleasedWhenClosed:NO];
        [window_ setTitlebarAppearsTransparent:YES];
        [window_ setTitleVisibility:NSWindowTitleHidden];
        [window_ setTitle:cocoa_string(desc.title)];

        view_ = [[NSView alloc] initWithFrame:[[window_ contentView] bounds]];
        [view_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        // A CAMetalLayer, not the default CALayer AppKit would make: it is what rhi.h's MetalLayer
        // contract promises a consumer, so the later GPU present path finds the layer it needs and
        // the CPU fallback (which only sets `contents`, a plain CALayer property) works on it now.
        layer_ = [CAMetalLayer layer];
        [layer_ setContentsGravity:kCAGravityResize];
        [view_ setLayer:layer_];
        [view_ setWantsLayer:YES];
        [window_ setContentView:view_];

        delegate_ = [[ContextShellWindowDelegate alloc] init];
        delegate_->mailbox = &mailbox_;
        [window_ setDelegate:delegate_];

        // Mouse-moved events are NOT delivered unless the window asks for them; without this the
        // browser never sees a hover.
        [window_ setAcceptsMouseMovedEvents:YES];

        cocoa_windows().push_back(this);

        if (desc.visible)
        {
            [window_ makeKeyAndOrderFront:nil];
            if (desc.placement.has_value() && desc.placement->maximized && ![window_ isZoomed])
            {
                [window_ zoom:nil];
            }
        }

        // Read the geometry ONCE, after any show: zooming changes it, and nothing before this point
        // reads size_ or geometry_.
        geometry_ = read_geometry();
        [layer_ setContentsScale:static_cast<CGFloat>(geometry_.dpi.factor())];
        // ns_extent_to_physical, NOT a hand-inlined copy of it: drain_geometry() below sets this same
        // size_ from translate_ns_window_geometry, which uses that function. Deriving the INITIAL size
        // any other way would put two implementations of one conversion behind one observable
        // (client_size(), read by attach_cpu/attach_gpu on the boot path), and the create()-time copy
        // is the one no CI leg runs.
        size_ = render::Extent2D{ns_extent_to_physical(geometry_.width_points, geometry_.dpi),
                                 ns_extent_to_physical(geometry_.height_points, geometry_.dpi)};
        modifier_flags_ = static_cast<std::uint64_t>([NSEvent modifierFlags]);
    }

    error.clear();
    return true;
}

void CocoaWindowBackend::destroy()
{
    if (window_ == nil)
    {
        return;
    }
    std::erase(cocoa_windows(), this);
    @autoreleasepool
    {
        // The delegate's mailbox points at a member of THIS object, so it is unhooked before the
        // window can deliver another callback into a dying backend.
        //
        // Guarded, unlike the message sends around it: `p->ivar = x` is a PLAIN POINTER
        // DEREFERENCE, not a message, so it does not get Objective-C's nil-receiver amnesty. The
        // early return above only proves window_ is non-nil, and window_ is assigned BEFORE the
        // delegate — so a create() that got a window but a nil delegate would crash here instead of
        // tearing down.
        [window_ setDelegate:nil];
        if (delegate_ != nil)
        {
            delegate_->mailbox = nullptr;
        }
        // d3: unhook the menu the same way the delegate is unhooked, and for the same reason —
        // every installed NSMenuItem's (unretained) target and its dispatch pointer aim into THIS
        // dying object. Detaching the bar from NSApp is what makes them unreachable from the UI;
        // nulling the target's pointer is the belt-and-braces for anything still holding the menu.
        if (menu_target_ != nil)
        {
            menu_target_->dispatch = nullptr;
        }
        if (main_menu_ != nil && [NSApp mainMenu] == main_menu_)
        {
            [NSApp setMainMenu:nil];
        }
        [window_ orderOut:nil];
        [window_ close];
    }
    delegate_ = nil;
    menu_target_ = nil;
    main_menu_ = nil;
    layer_ = nil;
    view_ = nil;
    window_ = nil;
}

bool CocoaWindowBackend::install_menu(const MenuModel& model, MenuActivationCallback on_activate)
{
    if (window_ == nil)
    {
        return false; // no live window — nothing owns an app menu bar here
    }
    @autoreleasepool
    {
        if (menu_target_ == nil)
        {
            menu_target_ = [ContextShellMenuTarget new];
            menu_target_->dispatch = &menu_dispatch_;
        }
        // The dispatch wraps the caller's callback with the activation counter, so the stats and
        // the relay can never disagree about how many activations happened.
        menu_dispatch_ = [this, callback = std::move(on_activate)](const std::string& id)
        {
            ++menu_activations_;
            if (callback)
            {
                callback(id);
            }
        };
        // Built WHOLESALE and swapped in one `setMainMenu:` — a re-publish is a re-render, the
        // mountChrome rule; there is no in-place item surgery to drift.
        NSMenu* bar = [[NSMenu alloc] initWithTitle:@"MainMenu"];
        [bar setAutoenablesItems:NO];
        std::size_t command_items = 0;
        for (const MenuDefinition& menu : model.menus)
        {
            NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:cocoa_string(menu.label)
                                                            action:nil
                                                     keyEquivalent:@""];
            [holder setSubmenu:build_ns_menu(menu.label, menu.items, menu_target_, command_items)];
            [bar addItem:holder];
        }
        [NSApp setMainMenu:bar];
        main_menu_ = bar;
        // What the BUILT tree holds — the builder's own tally, so the stat cannot drift from the
        // items actually installed.
        menu_items_ = command_items;
    }
    ++menu_installs_;
    return true;
}

bool CocoaWindowBackend::perform_menu(const std::string& command_id)
{
    if (main_menu_ == nil)
    {
        return false;
    }
    @autoreleasepool
    {
        NSMenuItem* item = find_ns_menu_item(main_menu_, cocoa_string(command_id));
        if (item == nil || ![item isEnabled])
        {
            // A DISABLED item is truly inert — the honesty the DoD pins: the programmatic path
            // refuses exactly where a real menu click could not fire.
            return false;
        }
        // Exactly what an activation invokes: the bound target/action pair, from the item itself.
        return [NSApp sendAction:[item action] to:[item target] from:item] == YES;
    }
}

NsWindowGeometry CocoaWindowBackend::read_geometry() const
{
    NsWindowGeometry geometry;
    if (window_ == nil || view_ == nil)
    {
        return geometry;
    }
    const NSRect bounds = [view_ bounds];
    geometry.width_points = static_cast<double>(bounds.size.width);
    geometry.height_points = static_cast<double>(bounds.size.height);
    const NSRect frame = [window_ frame];
    geometry.x = cocoa_round_i32(static_cast<double>(frame.origin.x));
    geometry.y = cocoa_round_i32(static_cast<double>(frame.origin.y));
    geometry.dpi = ns_dpi_from_backing_scale(static_cast<double>([window_ backingScaleFactor]));
    return geometry;
}

bool CocoaWindowBackend::hybrid_chrome(CocoaChromeState& out) const
{
    if (window_ == nil)
    {
        return false;
    }
    @autoreleasepool
    {
        // INTERIM HONESTY, MADE STRUCTURAL (tasks/README.md): `chrome.state.mode` reports what the
        // window actually DOES, so hybrid is answered only while the hybrid style really is live
        // on the NSWindow — read back at call time, never assumed from "create() set it". Revert
        // the style and this returns false, which the composition root serves as `system`/0.
        if (([window_ styleMask] & NSWindowStyleMaskFullSizeContentView) == 0 ||
            [window_ titlebarAppearsTransparent] == NO)
        {
            return false;
        }
        // The traffic-light cluster, MEASURED from the standard buttons' real frames (02 §4) —
        // never a hardcoded 70-something points, which drifts with the OS release and with the
        // user's accessibility sizing. Frames are converted to WINDOW coordinates through each
        // button's superview (the titlebar container view, NOT the content view); only the x
        // extent matters for a horizontal inset, so Cocoa's y flip is irrelevant here.
        double min_x = 0.0;
        double max_x = 0.0;
        bool any_button = false;
        const NSWindowButton kinds[] = {NSWindowCloseButton, NSWindowMiniaturizeButton,
                                        NSWindowZoomButton};
        for (const NSWindowButton kind : kinds)
        {
            NSButton* button = [window_ standardWindowButton:kind];
            if (button == nil || [button superview] == nil)
            {
                continue;
            }
            const NSRect frame = [[button superview] convertRect:[button frame] toView:nil];
            const double left = static_cast<double>(NSMinX(frame));
            const double right = static_cast<double>(NSMaxX(frame));
            if (!any_button || left < min_x)
            {
                min_x = left;
            }
            if (!any_button || right > max_x)
            {
                max_x = right;
            }
            any_button = true;
        }
        out = CocoaChromeState{};
        if (any_button)
        {
            // With FullSizeContentView the content view spans the full frame, so its width IS the
            // width the a2 strip lays out against — the right denominator for the RTL-side test.
            const double width =
                view_ != nil ? static_cast<double>([view_ bounds].size.width)
                             : static_cast<double>([window_ frame].size.width);
            out = ns_hybrid_controls_inset(
                min_x, max_x, width,
                ns_dpi_from_backing_scale(static_cast<double>([window_ backingScaleFactor])));
        }
        // A window with no standard buttons at all still HAS hybrid chrome (content under a
        // transparent titlebar); its honest inset is the {0, 0} `out` was reset to above.
    }
    return true;
}

bool CocoaWindowBackend::handle(NSEvent* event)
{
    const NSEventType type = [event type];
    NsEvent flat;
    flat.type = static_cast<std::int32_t>(type);

    switch (type)
    {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
    case NSEventTypeMouseMoved:
    case NSEventTypeScrollWheel:
    {
        // -locationInWindow is in WINDOW coordinates; the seam speaks VIEW coordinates, and the two
        // differ by the titlebar. Converting here (an AppKit call) keeps the flip + scale pure.
        const NSPoint in_view = [view_ convertPoint:[event locationInWindow] fromView:nil];
        flat.location_x = static_cast<double>(in_view.x);
        flat.location_y = static_cast<double>(in_view.y);
        flat.modifier_flags = static_cast<std::uint64_t>([event modifierFlags]);
        flat.pressed_mouse_buttons = static_cast<std::uint32_t>([NSEvent pressedMouseButtons]);
        if (type == NSEventTypeScrollWheel)
        {
            flat.scrolling_delta_x = static_cast<double>([event scrollingDeltaX]);
            flat.scrolling_delta_y = static_cast<double>([event scrollingDeltaY]);
            flat.has_precise_scrolling_deltas = [event hasPreciseScrollingDeltas] != NO;
        }
        else if (type != NSEventTypeMouseMoved)
        {
            // -clickCount is only defined for the press/release/drag family; asking a
            // NSEventTypeMouseMoved for it raises.
            flat.click_count = static_cast<std::int32_t>([event clickCount]);
            flat.button_number = static_cast<std::int32_t>([event buttonNumber]);
        }
        break;
    }
    case NSEventTypeMouseExited:
        flat.modifier_flags = static_cast<std::uint64_t>([event modifierFlags]);
        flat.pressed_mouse_buttons = static_cast<std::uint32_t>([NSEvent pressedMouseButtons]);
        break;
    case NSEventTypeKeyDown:
    case NSEventTypeKeyUp:
    case NSEventTypeFlagsChanged:
    {
        flat.modifier_flags = static_cast<std::uint64_t>([event modifierFlags]);
        flat.key_code = static_cast<std::uint32_t>([event keyCode]);
        if (type != NSEventTypeFlagsChanged)
        {
            // -characters raises on a FlagsChanged. The FIRST UTF-16 code unit, which is exactly
            // what the Win32 decoder forwards out of WM_CHAR's wParam — one convention for CEF on
            // every platform. A commit that produced more is truncated rather than concatenated
            // into one bogus code point, the same choice x11_window.cpp records.
            NSString* characters = [event characters];
            if (characters != nil && [characters length] > 0)
            {
                flat.text = static_cast<char32_t>([characters characterAtIndex:0]);
            }
        }
        break;
    }
    default:
        // Everything else — periodic, app-defined, tablet, cursor-update, the AppKit-internal
        // families. Not decoded, and (unlike the cases above) not even inspected: asking an
        // arbitrary NSEvent for a mouse or key property raises.
        return false;
    }

    const NsViewGeometry view_geometry{geometry_.height_points, geometry_.dpi, modifier_flags_};
    const ShellEventBatch batch = translate_ns_event(flat, view_geometry);
    // Recorded for the NEXT FlagsChanged diff, and only for the event types whose mask was actually
    // read — the `default:` arm above returns before reaching here rather than storing a zero.
    modifier_flags_ = flat.modifier_flags;

    // THE c1 CAPTION CONSULT (target design 02 §4). A decoded LEFT PRESS that lands in a published
    // `caption` region is the OS's, not the browser's: the single press hands THIS NSEvent to
    // performWindowDragWithEvent: (the OS then owns the drag, exactly as HTCAPTION does on
    // Windows), the double-click is zoom: — and either way the press is consumed WHOLE, right
    // here, so no ShellEvent is queued (the browser never sees a half-press to get a stuck hover
    // from — ROADMAP risk 3, the same claim b1 makes NC-side) and the pump skips its
    // [NSApp sendEvent:] forward for it (the hand-off IS the event's AppKit consumption; see the
    // pump). This must run at NSEvent time, not at dispatch time, because only here does the real
    // NSEvent still exist. The decision itself is the pure caption_press_action, leg-tested as
    // THE ARBITER'S OWN VERDICT: the same last-match-wins hit-test route_pointer uses AND the same
    // live capture state (InputArbiter::preview_pointer, side-effect free), so the two agree about
    // who owns EVERY press — a left press while another button's implicit capture or a modal
    // push_capture is live is `yielded` here and flows on to route_pointer, which routes it to the
    // capture target or swallows it for the modal backdrop, exactly as the preview said. Every
    // other event — moves, releases, keys, the right button — flows on untouched.
    if (type == NSEventTypeLeftMouseDown && caption_arbiter_ != nullptr && window_ != nil)
    {
        for (std::size_t i = 0; i < batch.count; ++i)
        {
            if (batch.events[i].kind != ShellEventKind::pointer)
            {
                continue;
            }
            switch (caption_press_action(batch.events[i].pointer, *caption_arbiter_))
            {
            // Both consumed arms ACTIVATE first: a titlebar click on a background window makes it
            // key and orders it front on native macOS, and that behaviour used to arrive through
            // the [NSApp sendEvent:] forward this consult now skips — neither
            // performWindowDragWithEvent: nor zoom: is documented to activate, so without the
            // explicit call a second editor window (e10 tear-outs) could be dragged or zoomed
            // while the keyboard stayed with its sibling. Idempotent for an already-key window.
            case CaptionPressAction::drag:
                ++caption_drags_;
                [window_ makeKeyAndOrderFront:nil];
                [window_ performWindowDragWithEvent:event];
                return true;
            case CaptionPressAction::double_click:
            {
                ++caption_double_clicks_;
                [window_ makeKeyAndOrderFront:nil];
                // The user's own title-bar double-click preference (cocoa_chrome.h
                // CaptionDoubleClickAction): read at the press, not cached, so a change in System
                // Settings applies to the next double-click like it does for every native window.
                NSString* preference = [[NSUserDefaults standardUserDefaults]
                    stringForKey:@"AppleActionOnDoubleClick"];
                const std::string value =
                    preference == nil ? std::string() : std::string([preference UTF8String]);
                switch (caption_double_click_action(value))
                {
                case CaptionDoubleClickAction::zoom:
                    [window_ zoom:nil];
                    break;
                case CaptionDoubleClickAction::minimize:
                    [window_ miniaturize:nil];
                    break;
                case CaptionDoubleClickAction::none:
                    break; // "Do nothing" — consumed all the same: it is still the caption's press
                }
                return true;
            }
            case CaptionPressAction::yielded:
                // Counted, NOT consumed: the press is enqueued and forwarded like any other, so
                // the arbiter applies the verdict the consult previewed.
                ++caption_yields_;
                break;
            case CaptionPressAction::none:
                break;
            }
            break; // one LeftMouseDown decodes at most one pointer fact; nothing more to consult
        }
    }

    for (std::size_t i = 0; i < batch.count; ++i)
    {
        pending_.push_back(batch.events[i]);
    }
    return false;
}

void CocoaWindowBackend::drain_geometry()
{
    const NsWindowGeometry current = read_geometry();
    const ShellEventBatch batch = translate_ns_window_geometry(geometry_, current);
    geometry_ = current;
    for (std::size_t i = 0; i < batch.count; ++i)
    {
        const ShellEvent& event = batch.events[i];
        if (event.kind == ShellEventKind::resize)
        {
            // The backend applies the state-changing events to ITSELF before handing them on,
            // exactly as the Win32 and X11 backends do — by the time a resize is observed,
            // client_size() is current.
            size_ = event.size;
        }
        else if (event.kind == ShellEventKind::dpi_changed && layer_ != nil)
        {
            // Without this the layer keeps rasterizing at the OLD scale, so a window dragged to a
            // Retina display shows a correctly-sized but visibly soft UI.
            [layer_ setContentsScale:static_cast<CGFloat>(event.dpi.factor())];
        }
        pending_.push_back(event);
    }
}

void CocoaWindowBackend::drain_mailbox()
{
    if (mailbox_.focus_gained)
    {
        mailbox_.focus_gained = false;
        ShellEvent event;
        event.kind = ShellEventKind::focus_gained;
        pending_.push_back(event);
    }
    if (mailbox_.focus_lost)
    {
        mailbox_.focus_lost = false;
        ShellEvent event;
        event.kind = ShellEventKind::focus_lost;
        pending_.push_back(event);
    }
    if (mailbox_.close_requested)
    {
        mailbox_.close_requested = false;
        alive_ = false;
        ShellEvent event;
        event.kind = ShellEventKind::close_requested;
        pending_.push_back(event);
    }
}

bool CocoaWindowBackend::pump(std::vector<ShellEvent>& out)
{
    @autoreleasepool
    {
        for (;;)
        {
            // distantPast, never a blocking wait: the owner loop also drives CEF and the compositor,
            // so it must never block inside the OS queue (03 §1 — the single-threaded owner loop).
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil)
            {
                break;
            }
            // Dispatched by owning window: one queue serves every window in the process, exactly
            // like the Win32 pump's NULL hwnd filter. An event with no window (an application-level
            // one) belongs to nobody here and is simply forwarded.
            NSWindow* owner = [event window];
            bool consumed = false;
            if (owner != nil)
            {
                for (CocoaWindowBackend* backend : cocoa_windows())
                {
                    if (backend != nullptr && backend->window() == owner)
                    {
                        consumed = backend->handle(event);
                        break;
                    }
                }
            }
            // ALWAYS forwarded, decoded or not — with EXACTLY ONE carve-out (editor-window-chrome
            // c1): a caption press handle() consumed was handed to performWindowDragWithEvent: /
            // zoom: INSTEAD, which is that event's AppKit consumption — forwarding it again would
            // dispatch the same press twice. For every other event the rule holds unchanged: the
            // Shell OBSERVES this stream, and swallowing it breaks window dragging, the menu, the
            // IME and every standard key equivalent (file header, shape 4).
            if (!consumed)
            {
                [NSApp sendEvent:event];
            }
        }
        drain_geometry();
        drain_mailbox();
    }
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
    return window_ != nil && alive_;
}

WindowPlacement CocoaWindowBackend::placement() const
{
    WindowPlacement placement;
    if (window_ == nil)
    {
        return placement;
    }
    @autoreleasepool
    {
        // ⚠ IN COCOA POINTS, WITH A BOTTOM-LEFT SCREEN ORIGIN — deliberately NOT converted to the
        // physical pixels + top-left origin the Win32 and X11 backends report. The document is
        // per-machine session state written and read by THIS backend alone, so points round-trip
        // exactly through apply_placement, while a conversion would need a screen height (making the
        // value wrong the moment the display arrangement changes) and would still not make a macOS
        // placement meaningful on another OS. Every consumer is symmetric; none compares across
        // platforms.
        const NSRect frame = [window_ frame];
        placement.x = cocoa_round_i32(static_cast<double>(frame.origin.x));
        placement.y = cocoa_round_i32(static_cast<double>(frame.origin.y));
        const NSRect content = [window_ contentRectForFrameRect:frame];
        placement.width =
            static_cast<std::uint32_t>(std::max(0, cocoa_round_i32(
                                                       static_cast<double>(content.size.width))));
        placement.height =
            static_cast<std::uint32_t>(std::max(0, cocoa_round_i32(
                                                       static_cast<double>(content.size.height))));
        placement.maximized = [window_ isZoomed] != NO;

        // The display's CGDirectDisplayID, not -localizedName: the name is user-facing and
        // LOCALIZED, so it is not an identity a later launch can match against.
        NSScreen* screen = [window_ screen];
        if (screen != nil)
        {
            NSNumber* number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
            if (number != nil)
            {
                placement.monitor =
                    std::string("cocoa:") + std::to_string([number unsignedLongLongValue]);
            }
        }
    }
    return placement;
}

void CocoaWindowBackend::apply_placement(const WindowPlacement& placement)
{
    if (window_ == nil || render::is_empty(placement.size()))
    {
        return;
    }
    @autoreleasepool
    {
        // Un-zoom FIRST when restoring: a zoomed window ignores a frame change, so applying the
        // restore rect while still zoomed silently does nothing — the same ordering rule the X11
        // backend records for _NET_WM_STATE_MAXIMIZED.
        if (!placement.maximized && [window_ isZoomed])
        {
            [window_ zoom:nil];
        }
        const NSRect content = NSMakeRect(static_cast<CGFloat>(placement.x),
                                          static_cast<CGFloat>(placement.y),
                                          static_cast<CGFloat>(placement.width),
                                          static_cast<CGFloat>(placement.height));
        [window_ setFrame:[window_ frameRectForContentRect:content] display:YES];
        if (placement.maximized && ![window_ isZoomed])
        {
            [window_ zoom:nil];
        }
    }
}

} // namespace

// --- the c1 hybrid-chrome query/wiring surface (cocoa_chrome.h) ----------------------------------
//
// Keyed on name() == "cocoa" — the one string only the real backend above answers — so the
// downcast to the anonymous-namespace type is confined to this TU and needs no RTTI. A headless
// backend (every mac CI smoke) and every other platform's backend answer the honest false/no-op.

namespace
{

// The one guard + downcast the three functions below share: non-null exactly when `backend` is the
// live CocoaWindowBackend defined in this TU.
const CocoaWindowBackend* as_cocoa(const IWindowBackend& backend)
{
    if (std::strcmp(backend.name(), "cocoa") != 0)
    {
        return nullptr;
    }
    return &static_cast<const CocoaWindowBackend&>(backend);
}

CocoaWindowBackend* as_cocoa(IWindowBackend& backend)
{
    // Safe const_cast: the referenced object arrived through a non-const reference.
    return const_cast<CocoaWindowBackend*>(as_cocoa(static_cast<const IWindowBackend&>(backend)));
}

} // namespace

bool cocoa_hybrid_chrome(const IWindowBackend& backend, CocoaChromeState& out)
{
    const CocoaWindowBackend* cocoa = as_cocoa(backend);
    return cocoa != nullptr && cocoa->hybrid_chrome(out);
}

void cocoa_bind_caption_arbiter(IWindowBackend& backend, const InputArbiter* arbiter)
{
    if (CocoaWindowBackend* cocoa = as_cocoa(backend))
    {
        cocoa->bind_caption_arbiter(arbiter);
    }
}

bool cocoa_pin_double_click_preference(const char* apple_value)
{
    if (apple_value == nullptr)
    {
        return false;
    }
    // The ARGUMENT domain: what `-AppleActionOnDoubleClick Maximize` on the command line would
    // set, and the highest-precedence domain NSUserDefaults consults — above the app's own domain
    // and the global (System Settings) domain — so the pump's `stringForKey:` sees this value on
    // any machine. Process-local and volatile: nothing is written to the user's preferences.
    [[NSUserDefaults standardUserDefaults]
        setVolatileDomain:@{@"AppleActionOnDoubleClick" : [NSString stringWithUTF8String:apple_value]}
                  forName:NSArgumentDomain];
    return true;
}

bool cocoa_caption_stats(const IWindowBackend& backend, CocoaCaptionStats& out)
{
    const CocoaWindowBackend* cocoa = as_cocoa(backend);
    if (cocoa == nullptr)
    {
        return false;
    }
    out = cocoa->caption_stats();
    return true;
}

// --- the d3 native-menu query/wiring surface (cocoa_menu.h) --------------------------------------
// Same name()-keyed downcast as the c1 surface above; every non-Cocoa backend (headless — every mac
// CI smoke — and the other platforms' backends) answers the honest false.

bool cocoa_install_menu(IWindowBackend& backend, const MenuModel& model,
                        MenuActivationCallback on_activate)
{
    CocoaWindowBackend* cocoa = as_cocoa(backend);
    return cocoa != nullptr && cocoa->install_menu(model, std::move(on_activate));
}

bool cocoa_menu_stats(const IWindowBackend& backend, CocoaMenuStats& out)
{
    const CocoaWindowBackend* cocoa = as_cocoa(backend);
    if (cocoa == nullptr)
    {
        return false;
    }
    out = cocoa->menu_stats();
    return true;
}

bool cocoa_menu_perform(IWindowBackend& backend, const std::string& command_id)
{
    CocoaWindowBackend* cocoa = as_cocoa(backend);
    return cocoa != nullptr && cocoa->perform_menu(command_id);
}

std::unique_ptr<IWindowBackend> make_cocoa_window_backend(const WindowDesc& desc,
                                                           std::string& error)
{
    auto backend = std::make_unique<CocoaWindowBackend>();
    if (!backend->create(desc, error))
    {
        return nullptr;
    }
    return backend;
}

} // namespace context::editor::shell

#endif // __APPLE__
