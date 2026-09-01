// Per-monitor DPI arithmetic — see dpi.h for why the scale is derived from the DPI rather than stored.

#include "context/editor/shell/dpi.h"

#include <algorithm>
#include <cmath>

namespace context::editor::shell
{
namespace
{

// Round-to-nearest on a non-negative scaled extent, never collapsing a non-empty input to 0.
[[nodiscard]] std::uint32_t scale_extent(std::uint32_t value, float factor)
{
    if (value == 0u)
    {
        return 0u;
    }
    const float scaled = static_cast<float>(value) * factor;
    const long rounded = std::lround(scaled);
    return rounded < 1 ? 1u : static_cast<std::uint32_t>(rounded);
}

// Round-to-nearest on a SIGNED coordinate. std::lround rounds halfway cases away from zero, which is
// the symmetric behaviour a coordinate wants (unlike the extent above, a position may be negative).
[[nodiscard]] std::int32_t scale_coord(std::int32_t value, float factor)
{
    return static_cast<std::int32_t>(std::lround(static_cast<float>(value) * factor));
}

} // namespace

DpiScale make_dpi_scale(std::uint32_t raw_dpi)
{
    return DpiScale{std::clamp(raw_dpi, kMinDpi, kMaxDpi)};
}

render::Extent2D to_physical(render::Extent2D logical, DpiScale scale)
{
    const float factor = scale.factor();
    return render::Extent2D{scale_extent(logical.width, factor),
                            scale_extent(logical.height, factor)};
}

render::Extent2D to_logical(render::Extent2D physical, DpiScale scale)
{
    const float inverse = 1.0f / scale.factor();
    return render::Extent2D{scale_extent(physical.width, inverse),
                            scale_extent(physical.height, inverse)};
}

PointI to_logical_point(PointI physical, DpiScale scale)
{
    const float inverse = 1.0f / scale.factor();
    return PointI{scale_coord(physical.x, inverse), scale_coord(physical.y, inverse)};
}

PointI to_physical_point(PointI logical, DpiScale scale)
{
    const float factor = scale.factor();
    return PointI{scale_coord(logical.x, factor), scale_coord(logical.y, factor)};
}

render::Rect2D osr_popup_dest_rect(const render::Rect2D& popup_dip, render::Extent2D physical_size,
                                   DpiScale scale)
{
    // to_physical_point, NOT to_physical: the origin is a COORDINATE, and this is the SAME
    // conversion `osr_screen_point` applies to a view offset — so the place the popup is drawn and
    // the place CEF is told the view maps to can never round differently.
    //
    // Honest about the difference: `to_physical`'s never-collapse clamp is UNREACHABLE here — it
    // only fires for a non-zero value scaling below 0.5, and the low DPI clamp is 0.5x, at which no
    // non-negative integer does that (verified over kMinDpi..kMaxDpi). So this is a contract choice,
    // not a behavioural one, and a test asserting otherwise would be asserting nothing.
    const PointI origin =
        to_physical_point(PointI{static_cast<std::int32_t>(popup_dip.origin.x),
                                 static_cast<std::int32_t>(popup_dip.origin.y)},
                          scale);
    render::Rect2D dest;
    // render::Rect2D's origin is unsigned. A DIP origin is unsigned and the factor is positive, so
    // the product cannot be negative in practice; clamping rather than casting keeps a wrapped
    // input from addressing the far end of the surface.
    dest.origin.x = origin.x > 0 ? static_cast<std::uint32_t>(origin.x) : 0u;
    dest.origin.y = origin.y > 0 ? static_cast<std::uint32_t>(origin.y) : 0u;
    // The SIZE comes from the texture, never from the DIP rect — see dpi.h for why the two are not
    // interchangeable even after scaling.
    dest.size = physical_size;
    return dest;
}

PointI osr_screen_point(PointI view_dip, PointI client_origin, DpiScale scale,
                        bool screen_coords_are_dip)
{
    // The OFFSET carries the per-platform split; the ORIGIN is already in the platform's own
    // screen convention, so it is added untouched. Scaling the sum instead would scale the origin
    // twice — the same class of double-application the root rect below must not make.
    const PointI offset = screen_coords_are_dip ? view_dip : to_physical_point(view_dip, scale);
    return PointI{client_origin.x + offset.x, client_origin.y + offset.y};
}

ScreenRect osr_root_screen_rect(PointI client_origin, render::Extent2D logical_size, DpiScale scale,
                                bool screen_coords_are_dip)
{
    // DIP OUT, on every platform (see dpi.h). The size is already DIP and passes through
    // UNSCALED — multiplying it by the scale factor is precisely the bug this member's wording
    // rules out. Only the origin is converted, and only where the platform hands it to us in
    // device pixels.
    const PointI origin_dip =
        screen_coords_are_dip ? client_origin : to_logical_point(client_origin, scale);
    return ScreenRect{origin_dip, logical_size};
}

PointI osr_view_point(PointI screen, PointI client_origin, DpiScale scale,
                      bool screen_coords_are_dip)
{
    // SUBTRACT IN THE SCREEN'S OWN CONVENTION, CONVERT THE OFFSET (see dpi.h). Both operands are in
    // the platform's screen units here, so the difference is too; converting `screen` first would
    // also convert the origin, which is the double application this pair exists to avoid.
    const PointI offset{screen.x - client_origin.x, screen.y - client_origin.y};
    return screen_coords_are_dip ? offset : to_logical_point(offset, scale);
}

} // namespace context::editor::shell
