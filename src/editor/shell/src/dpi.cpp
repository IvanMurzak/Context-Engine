// Per-monitor DPI arithmetic — see dpi.h for why the scale is derived from the DPI rather than stored.

#include "context/editor/shell/dpi.h"

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
    DpiScale scale;
    if (raw_dpi < kMinDpi)
    {
        scale.dpi = kMinDpi;
    }
    else if (raw_dpi > kMaxDpi)
    {
        scale.dpi = kMaxDpi;
    }
    else
    {
        scale.dpi = raw_dpi;
    }
    return scale;
}

render::Extent2D to_physical(render::Extent2D logical, DpiScale scale)
{
    const float factor = scale.factor();
    return render::Extent2D{scale_extent(logical.width, factor),
                            scale_extent(logical.height, factor)};
}

render::Extent2D osr_screen_extent(render::Extent2D logical, DpiScale scale,
                                   bool screen_rect_is_dip)
{
    return screen_rect_is_dip ? logical : to_physical(logical, scale);
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

} // namespace context::editor::shell
