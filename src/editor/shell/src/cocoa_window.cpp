// The OFF-PLATFORM half of the macOS window backend (M9 e12b).
//
// Its whole body is the refusals `make_cocoa_window_backend` — and, since editor-window-chrome c1,
// the cocoa_chrome.h query/wiring surface — must still be linkable SYMBOLS for on Windows and
// Linux, so the ctest can assert the off-platform behaviour as a VALUE on every leg rather than not
// being able to name the functions at all — the same shape make_win32_gdi_blitter established and
// make_win32_window_backend / make_x11_window_backend follow.
//
// It is a separate file from cocoa_window.mm (rather than that file's `#else`, which is how the
// Win32 and X11 backends are arranged) for one reason: Objective-C++ needs the OBJCXX language, and
// CMake picks a compiler per FILE EXTENSION. A single `.mm` carrying both halves would have to be
// compiled as Objective-C++ on Linux and Windows too, where no OBJCXX compiler is enabled at all.

#include "context/editor/shell/window.h"

#if !defined(__APPLE__)

#include "context/editor/shell/cocoa_chrome.h"

#include <memory>
#include <string>

namespace context::editor::shell
{

std::unique_ptr<IWindowBackend> make_cocoa_window_backend(const WindowDesc& /*desc*/,
                                                           std::string& error)
{
    error = "the Cocoa window backend is compiled only on macOS";
    return nullptr;
}

// The c1 hybrid-chrome query/wiring surface (cocoa_chrome.h), same linkable-symbol rule as the
// factory above: off macOS no backend can be the Cocoa one, so the query REFUSES and the wiring is
// a no-op — behaviour the ctest asserts as a VALUE on every leg rather than being unable to name
// the functions at all. (The pure halves — caption_press_action / ns_hybrid_controls_inset — are
// platform-free and live in cocoa_chrome.cpp, compiled everywhere.)

bool cocoa_hybrid_chrome(const IWindowBackend& /*backend*/, CocoaChromeState& /*out*/)
{
    return false;
}

void cocoa_bind_caption_regions(IWindowBackend& /*backend*/, const RegionMap* /*regions*/) {}

bool cocoa_caption_stats(const IWindowBackend& /*backend*/, CocoaCaptionStats& /*out*/)
{
    return false;
}

} // namespace context::editor::shell

#endif // !__APPLE__
