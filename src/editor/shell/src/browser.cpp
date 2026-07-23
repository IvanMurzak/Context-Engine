// The portable half of the browser seam: the scripted host and the premultiplied-BGRA producer both
// the smoke and the tests drive the compositor with. See browser.h.

#include "context/editor/shell/browser.h"

#include <utility>

namespace context::editor::shell
{

// ------------------------------------------------------------------------------- PumpSchedule

void PumpSchedule::schedule(std::int64_t delay_ms, std::int64_t now_ms)
{
    const std::int64_t clamped = delay_ms < 0 ? 0 : delay_ms;
    // Store the deadline BEFORE publishing the flag: the owner thread reads the flag first, so this
    // order is what guarantees it never observes `scheduled` true against a stale deadline.
    due_ms_.store(now_ms + clamped, std::memory_order_relaxed);
    scheduled_.store(true, std::memory_order_release);
}

bool PumpSchedule::should_pump(std::int64_t now_ms)
{
    if (!scheduled_.load(std::memory_order_acquire))
    {
        return true; // the floor
    }
    if (now_ms < due_ms_.load(std::memory_order_relaxed))
    {
        return false;
    }
    scheduled_.store(false, std::memory_order_relaxed);
    return true;
}

bool PumpSchedule::has_scheduled_work() const
{
    return scheduled_.load(std::memory_order_acquire);
}

std::int64_t PumpSchedule::due_ms() const
{
    return due_ms_.load(std::memory_order_relaxed);
}

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

void ScriptedBrowserHost::execute_script(std::string_view source)
{
    // RECORDED, never run: this host has no JavaScript engine. Silently dropping the call would let
    // a test believe a script executed here — and the one caller that matters (the popup-suppression
    // proof) is only meaningful against a real renderer, which is why it lives in the live CEF smoke.
    scripts_.emplace_back(source);
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
        const std::uint32_t tight = step.coded_size.width * 4u;
        out.frame.pixels = step.pixels.data();
        out.frame.byte_size = step.pixels.size();
        out.frame.bytes_per_row = step.bytes_per_row < tight ? tight : step.bytes_per_row;
        out.frame.coded_size = step.coded_size;
        out.frame.visible_rect = step.visible_rect;
        out.frame.dirty = step.dirty;
        sink.on_browser_frame(out);
    }
    return alive_;
}

void ScriptedBrowserHost::queue_frame(BrowserLayer layer, render::Extent2D coded_size,
                                      const render::Rect2D& visible_rect,
                                      std::vector<std::uint8_t> pixels, std::uint32_t bytes_per_row,
                                      std::vector<render::Rect2D> dirty)
{
    Step step;
    step.layer = layer;
    step.coded_size = coded_size;
    step.visible_rect = visible_rect;
    step.bytes_per_row = bytes_per_row;
    step.pixels = std::move(pixels);
    step.dirty = std::move(dirty);
    steps_.push_back(std::move(step));
}

void ScriptedBrowserHost::queue_solid_frame(BrowserLayer layer, render::Extent2D coded_size,
                                            const render::Rect2D& visible_rect, std::uint8_t b,
                                            std::uint8_t g, std::uint8_t r, std::uint8_t a,
                                            std::uint32_t bytes_per_row)
{
    queue_frame(layer, coded_size, visible_rect,
                make_premultiplied_bgra(coded_size, bytes_per_row, b, g, r, a), bytes_per_row);
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
