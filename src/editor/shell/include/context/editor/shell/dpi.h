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

} // namespace context::editor::shell
