// The OFF-PLATFORM half of the macOS window backend (M9 e12b).
//
// Its whole body is the refusal `make_cocoa_window_backend` must still be a linkable SYMBOL for on
// Windows and Linux, so `editor-shell-test_window` can assert the off-platform behaviour as a VALUE
// on every leg rather than not being able to name the function at all — the same shape
// make_win32_gdi_blitter established and make_win32_window_backend / make_x11_window_backend follow.
//
// It is a separate file from cocoa_window.mm (rather than that file's `#else`, which is how the
// Win32 and X11 backends are arranged) for one reason: Objective-C++ needs the OBJCXX language, and
// CMake picks a compiler per FILE EXTENSION. A single `.mm` carrying both halves would have to be
// compiled as Objective-C++ on Linux and Windows too, where no OBJCXX compiler is enabled at all.

#include "context/editor/shell/window.h"

#if !defined(__APPLE__)

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

} // namespace context::editor::shell

#endif // !__APPLE__
