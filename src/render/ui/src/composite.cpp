// Composite-time UI math — see context/render/ui/composite.h.

#include "context/render/ui/composite.h"

#include <algorithm>

namespace context::render::ui
{

packages::ui::Rect apply_transform(const packages::ui::Rect& bounds,
                                   const packages::ui::Transform& transform) noexcept
{
    const float sw = bounds.w * transform.scale.x;
    const float sh = bounds.h * transform.scale.y;
    if (sw <= 0.0f || sh <= 0.0f)
    {
        return packages::ui::Rect{}; // collapsed — nothing to draw
    }
    const float cx = bounds.x + bounds.w * 0.5f + transform.translate.x;
    const float cy = bounds.y + bounds.h * 0.5f + transform.translate.y;
    return packages::ui::Rect{cx - sw * 0.5f, cy - sh * 0.5f, sw, sh};
}

float effective_opacity(float ancestor_opacity, float node_opacity) noexcept
{
    return std::clamp(ancestor_opacity * node_opacity, 0.0f, 1.0f);
}

packages::ui::Color blend_over(const packages::ui::Color& backdrop, const packages::ui::Color& src,
                               float opacity) noexcept
{
    const float a = std::clamp(static_cast<float>(src.a) / 255.0f * opacity, 0.0f, 1.0f);
    auto mix = [a](std::uint8_t back, std::uint8_t front)
    {
        const float v = static_cast<float>(back) * (1.0f - a) + static_cast<float>(front) * a;
        return static_cast<std::uint8_t>(v + 0.5f);
    };
    return packages::ui::Color{mix(backdrop.r, src.r), mix(backdrop.g, src.g), mix(backdrop.b, src.b),
                               255};
}

} // namespace context::render::ui
