// Per-monitor DPI for the Shell (design 03 §1) — the arithmetic that replaces the spike's DPI-1.0 pin.
//
// Three consumers need the SAME number and would otherwise each derive it: the swapchain (physical
// backbuffer pixels), CEF (`device_scale_factor` + the view rect it reports in DIP), and the input
// pump (an OS pointer position is physical; a browser mouse event is DIP). A per-monitor-v2 window
// changes this number while running — dragged to a second monitor, or the user changing scaling —
// so it is a live value threaded through the frame, not a boot-time constant.
//
// The DPI is the stored value and the scale factor is DERIVED from it. Storing both is the classic
// pair that drifts: Windows hands us an integer DPI (WM_DPICHANGED), CEF wants the float, and a
// struct carrying two independently-settable fields is a struct that can disagree with itself.

#pragma once

#include "context/render/rhi.h"

#include <cstdint>

namespace context::editor::shell
{

// Windows' reference DPI: 96 dpi == scale 1.0 == 1 DIP is 1 physical pixel.
inline constexpr std::uint32_t kReferenceDpi = 96u;

// The clamp range. A monitor reporting 0 dpi would divide the whole layout to nothing, and a wildly
// large value would allocate a backbuffer no GPU will configure — both are reported-by-the-OS values
// we do not control, so they are clamped at the seam rather than trusted.
inline constexpr std::uint32_t kMinDpi = 48u;   // 0.5x
inline constexpr std::uint32_t kMaxDpi = 960u;  // 10x

// One monitor's scale. `dpi` is the source of truth; `factor()` is derived.
struct DpiScale
{
    std::uint32_t dpi = kReferenceDpi;

    [[nodiscard]] float factor() const
    {
        return static_cast<float>(dpi) / static_cast<float>(kReferenceDpi);
    }

    [[nodiscard]] bool operator==(const DpiScale& other) const { return dpi == other.dpi; }
    [[nodiscard]] bool operator!=(const DpiScale& other) const { return dpi != other.dpi; }
};

// Clamp a raw OS-reported DPI into the supported range (see kMinDpi/kMaxDpi).
[[nodiscard]] DpiScale make_dpi_scale(std::uint32_t raw_dpi);

// Logical (DIP) -> physical pixels, round-to-nearest.
//
// A non-empty logical extent NEVER becomes empty: a 1x1 logical window at 0.5x would round to 0,
// and a zero extent is IGNORED by ISwapchain::resize (a minimized window reports one every frame),
// so the swapchain would silently keep a stale size while the window really did change. Clamping to
// 1 keeps "empty means empty" true.
[[nodiscard]] render::Extent2D to_physical(render::Extent2D logical, DpiScale scale);

// Physical pixels -> logical (DIP), round-to-nearest, with the same never-collapse rule. This is the
// direction that matters most: the OS hands the Shell a physical client rect, and CEF's view rect
// (GetViewRect) must be reported in DIP or the browser lays out at the wrong size.
[[nodiscard]] render::Extent2D to_logical(render::Extent2D physical, DpiScale scale);

// A physical pointer position -> DIP, for a browser mouse event. Signed because a captured pointer
// legitimately travels outside the client area (a drag past the window edge), and because rounding
// must go toward zero symmetrically rather than flooring negatives away from it.
struct PointI
{
    std::int32_t x = 0;
    std::int32_t y = 0;

    [[nodiscard]] bool operator==(const PointI& other) const { return x == other.x && y == other.y; }
};

[[nodiscard]] PointI to_logical_point(PointI physical, DpiScale scale);
[[nodiscard]] PointI to_physical_point(PointI logical, DpiScale scale);

// ------------------------------------------------------------------ the OSR popup rect (a2)
//
// WHERE THE `PET_POPUP` LAYER IS COMPOSITED. Read verbatim from the pinned `cef_render_handler.h`
// (`tools/cef-prebuilt.json` -> CEF 149.0.6+g0d0eeb6+chromium-149.0.7827.201), the two members that
// describe the popup, which do NOT share a unit:
//
//   * `OnPopupSize` (:124-130) — "|rect| contains the new location and size in view coordinates" —
//     and `GetViewRect` (:80-85) defines view coordinates as DIP.
//   * `OnPaint` (:132-142) — "Pixel values passed to this method are scaled relative to view
//     coordinates based on the value of CefScreenInfo.device_scale_factor" — so the popup's own
//     TEXTURE is PHYSICAL, exactly like the view layer's.
//
// Compositing the DIP rect as if it were the physical destination is what put a 150 % `<select>` up
// and left of where it was drawn, at two thirds the size (cropped on the CPU path, squeezed on the
// GPU one) — and, because CEF hit-tests the TRUE DIP rect, made a click on the drawn popup land
// outside it and dismiss the menu. Like every bug in this family it is invisible at 1.0, where the
// two units coincide.
//
// THE ASYMMETRY IS THE FIX, and it is why this takes a size rather than deriving one:
//
//   * the ORIGIN is converted DIP -> physical, because it addresses the same surface the physical
//     view frame is composited into;
//   * the SIZE is NOT converted. `physical_size` is the popup TEXTURE's own visible extent. CEF
//     paints ceil(DIP x scale) pixels for a DIP rect, which round(DIP x scale) does not always
//     reproduce (1 DIP at 1.25x is 2 pixels painted and 1 rounded), and a destination one pixel off
//     the texture crops a column of the dropdown or resamples the whole thing. The texture is the
//     ground truth for HOW BIG the popup is; the DIP rect is the ground truth for WHERE it is.
[[nodiscard]] render::Rect2D osr_popup_dest_rect(const render::Rect2D& popup_dip,
                                                 render::Extent2D physical_size, DpiScale scale);

// ------------------------------------------------------- the OSR screen mapping (a1, audit D12)
//
// WHERE THE VIEW SITS ON SCREEN. An OSR browser gets none of this from the OS: the host must
// answer it, and CEF asks through two `CefRenderHandler` members that do NOT share a coordinate
// convention. Read verbatim from the pinned `cef_render_handler.h`
// (`tools/cef-prebuilt.json` -> CEF 149.0.6+g0d0eeb6+chromium-149.0.7827.201):
//
//   * `GetScreenPoint` (:87-100) — "the translation from view DIP coordinates to screen
//     coordinates. Windows/Linux should provide screen device (pixel) coordinates and MacOS should
//     provide screen DIP coordinates" — the ONE member that carries the per-platform split.
//   * `GetRootScreenRect` (:70-78) — "the root window rectangle in screen DIP coordinates" —
//     DIP on EVERY platform, no split.
//
// APPLYING THE SPLIT TO BOTH IS THE MISTAKE THIS PAIR EXISTS TO PREVENT: it multiplies the root
// rect by the scale factor on Windows/Linux, and — like every other bug in this family — is
// invisible at scale 1.0, where the two conventions coincide. (a2 removed the third caller of that
// split, `GetScreenInfo::rect`, for the same reason — see `cef_shell.cpp`'s `GetScreenInfo`.)
//
// Both take the window's CLIENT origin on screen, in the platform's own screen convention (the
// same predicate as `screen_rect_is_dip` above: device pixels on Windows/Linux, DIP on macOS).
// THE CLIENT ORIGIN, NEVER THE WINDOW RECT: a frameless window's client is inset from its window
// rect (`win32_frameless_client_insets` in window.h), and feeding the window origin here puts
// every context menu that inset away from the cursor. `IWindowBackend::client_origin()` is the one
// source of that value — each backend answers it from the live OS, so a MAXIMIZED window (whose
// persisted `WindowPlacement` holds the RESTORE rect, not where it currently is) stays correct.

// A screen rectangle with a SIGNED origin. `render::Rect2D`'s origin is UNSIGNED — it is a texel
// rect — and cannot express a window on a monitor left of or above the primary one, which is an
// ordinary multi-monitor arrangement rather than an edge case; a clamping conversion would move
// such a window silently to 0 instead of reporting where it is.
struct ScreenRect
{
    PointI origin;
    render::Extent2D size;

    [[nodiscard]] bool operator==(const ScreenRect& other) const
    {
        return origin == other.origin && size.width == other.size.width &&
               size.height == other.size.height;
    }
};

// View DIP -> screen, for `CefRenderHandler::GetScreenPoint`. The offset from the client origin is
// scaled to DEVICE pixels wherever the platform's screen coordinates are device pixels, and passed
// through untouched where they are DIP.
[[nodiscard]] PointI osr_screen_point(PointI view_dip, PointI client_origin, DpiScale scale,
                                      bool screen_coords_are_dip);

// The root window rect for `CefRenderHandler::GetRootScreenRect` — ALWAYS DIP, on every platform.
//
// Note the asymmetry, which is the whole point: the OUTPUT never scales (`logical_size` is already
// DIP and is returned as-is), while the INPUT origin is converted DOWN to DIP on the platforms
// whose screen coordinates are device pixels. Multiplying the size by the scale factor is the error
// the header's wording rules out.
//
// The rect reported is the CLIENT rect on screen rather than the outer window rect: the header
// defines `GetViewRect` as this member's fallback ("if this method returns false the rectangle
// from GetViewRect will be used"), so the honest correction of that fallback for a windowless
// browser whose view IS the whole client area is the same rect, moved to where it really is. That
// also keeps the two members mutually consistent by construction — `osr_screen_point({0, 0}, …)`
// is exactly this rect's origin expressed in the platform's own convention.
[[nodiscard]] ScreenRect osr_root_screen_rect(PointI client_origin, render::Extent2D logical_size,
                                              DpiScale scale, bool screen_coords_are_dip);

#if defined(_WIN32)

// The resolution state of the dynamically-loaded user32 DPI entry points (win32_window.cpp).
//
// Exposed as a seam for ONE regression: `context_editor` links with CEF's standard
// `/DELAYLOAD:user32.dll`, so user32 is NOT resident when the resolver runs (the resolver runs
// before the process has created a window or called any user32 function — that is its job), and a
// `GetModuleHandleW`-based lookup returns null there. That null resolved every entry point below to
// nullptr and silently pinned the whole shell to 96 DPI — a 150% desktop rendered the editor at
// 1.0x, with the un-covered band of the window left black. `editor-shell-test_win32_dpi` rebuilds
// that exact link condition (delay-loaded user32, no prior user32 call) and requires resolution to
// succeed anyway.
struct Win32DpiApiStatus
{
    bool set_process_dpi_awareness_context = false;
    bool get_dpi_for_window = false;
    bool adjust_window_rect_ex_for_dpi = false;
};

[[nodiscard]] Win32DpiApiStatus win32_dpi_api_status();

// Applies DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 to the process. True when THIS call set it;
// false when the entry point is missing (pre-1703 Windows) or the process awareness was already
// fixed (a call can only succeed once per process — CEF also sets it during CefInitialize, which is
// why the window backend applies it as early as it can). Callers treat false as non-fatal.
[[nodiscard]] bool win32_apply_per_monitor_dpi_awareness();

#endif // _WIN32

} // namespace context::editor::shell
