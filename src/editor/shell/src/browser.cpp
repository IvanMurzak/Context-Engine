// The portable half of the browser seam: the scripted host and the premultiplied-BGRA producer both
// the smoke and the tests drive the compositor with. See browser.h.

#include "context/editor/shell/browser.h"

#include <utility>

namespace context::editor::shell
{

std::vector<std::uint8_t> make_premultiplied_bgra(render::Extent2D coded_size,
                                                  std::uint32_t bytes_per_row, std::uint8_t b,
                                                  std::uint8_t g, std::uint8_t r, std::uint8_t a)
{
    const std::uint32_t tight = coded_size.width * 4u;
    const std::uint32_t stride = bytes_per_row < tight ? tight : bytes_per_row;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * coded_size.height, 0u);
    for (std::uint32_t y = 0; y < coded_size.height; ++y)
    {
        std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < coded_size.width; ++x)
        {
            std::uint8_t* texel = row + static_cast<std::size_t>(x) * 4u;
            texel[0] = b;
            texel[1] = g;
            texel[2] = r;
            texel[3] = a;
        }
    }
    return pixels;
}

void ScriptedBrowserHost::resize(render::Extent2D logical_size, DpiScale dpi)
{
    last_logical_size_ = logical_size;
    last_dpi_ = dpi;
    ++resize_count_;
}

void ScriptedBrowserHost::send_pointer(const PointerDispatch& /*dispatch*/,
                                       const PointerEvent& event)
{
    pointers_.push_back(event);
}

void ScriptedBrowserHost::send_key(const KeyEvent& event)
{
    keys_.push_back(event);
}

void ScriptedBrowserHost::set_focus(bool focused)
{
    focused_ = focused;
}

bool ScriptedBrowserHost::pump(IBrowserFrameSink& sink)
{
    // Moved out before delivery: a sink that queues more work (a popup opening in response to a
    // paint) must not mutate the vector being iterated.
    std::vector<Step> steps = std::move(steps_);
    steps_.clear();
    for (const Step& step : steps)
    {
        if (step.is_popup_state)
        {
            sink.on_popup_state(step.popup_visible, step.popup_rect);
            continue;
        }
        BrowserFrame out;
        out.layer = step.layer;
        out.frame.pixels = step.pixels.data();
        out.frame.byte_size = step.pixels.size();
        out.frame.bytes_per_row = step.coded_size.width * 4u;
        out.frame.coded_size = step.coded_size;
        out.frame.visible_rect = step.visible_rect;
        out.frame.dirty = step.dirty;
        sink.on_browser_frame(out);
    }
    return alive_;
}

void ScriptedBrowserHost::queue_frame(BrowserLayer layer, render::Extent2D coded_size,
                                      const render::Rect2D& visible_rect,
                                      std::vector<std::uint8_t> pixels,
                                      std::vector<render::Rect2D> dirty)
{
    Step step;
    step.layer = layer;
    step.coded_size = coded_size;
    step.visible_rect = visible_rect;
    step.pixels = std::move(pixels);
    step.dirty = std::move(dirty);
    steps_.push_back(std::move(step));
}

void ScriptedBrowserHost::queue_solid_frame(BrowserLayer layer, render::Extent2D coded_size,
                                            const render::Rect2D& visible_rect, std::uint8_t b,
                                            std::uint8_t g, std::uint8_t r, std::uint8_t a)
{
    queue_frame(layer, coded_size, visible_rect,
                make_premultiplied_bgra(coded_size, coded_size.width * 4u, b, g, r, a));
}

void ScriptedBrowserHost::queue_popup_state(bool visible, const render::Rect2D& rect)
{
    Step step;
    step.is_popup_state = true;
    step.popup_visible = visible;
    step.popup_rect = rect;
    steps_.push_back(std::move(step));
}

} // namespace context::editor::shell
