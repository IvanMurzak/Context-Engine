// The Shell's input arbitration (design 03 §6) — see input.h for the five decisions and their order.

#include "context/editor/shell/input.h"

namespace context::editor::shell
{
namespace
{

// Is a physical point inside a rect? Rect2D carries UNSIGNED origin/extent while a pointer position
// is signed, so the comparison is done in the signed domain — casting the point up to unsigned would
// wrap a negative coordinate (a captured drag past the window's left edge) into an enormous positive
// one that lands inside almost any rect.
[[nodiscard]] bool contains(const render::Rect2D& rect, PointI point)
{
    if (render::is_empty(rect.size))
    {
        return false;
    }
    const std::int64_t x0 = static_cast<std::int64_t>(rect.origin.x);
    const std::int64_t y0 = static_cast<std::int64_t>(rect.origin.y);
    const std::int64_t x1 = x0 + static_cast<std::int64_t>(rect.size.width);
    const std::int64_t y1 = y0 + static_cast<std::int64_t>(rect.size.height);
    const std::int64_t px = static_cast<std::int64_t>(point.x);
    const std::int64_t py = static_cast<std::int64_t>(point.y);
    return px >= x0 && px < x1 && py >= y0 && py < y1;
}

[[nodiscard]] InputTarget target_for(RegionKind kind)
{
    return kind == RegionKind::viewport ? InputTarget::viewport : InputTarget::native;
}

} // namespace

// ------------------------------------------------------------------------------------- RegionMap

void RegionMap::publish(std::vector<ShellRegion> regions)
{
    regions_ = std::move(regions);
    ++generation_;
}

const ShellRegion* RegionMap::hit_test(PointI physical) const
{
    // Back-to-front: the LAST match wins, so a later entry is stacked above an earlier one.
    for (std::size_t i = regions_.size(); i > 0; --i)
    {
        const ShellRegion& region = regions_[i - 1];
        if (contains(region.rect, physical))
        {
            return &region;
        }
    }
    return nullptr;
}

const ShellRegion* RegionMap::find(const std::string& id) const
{
    for (const ShellRegion& region : regions_)
    {
        if (region.id == id)
        {
            return &region;
        }
    }
    return nullptr;
}

// ----------------------------------------------------------------------------------- InputArbiter

void InputArbiter::set_keymap_resolver(KeymapResolver resolver, void* user_data)
{
    keymap_resolver_ = resolver;
    keymap_user_data_ = user_data;
}

void InputArbiter::push_capture(const Capture& capture)
{
    captures_.push_back(capture);
}

bool InputArbiter::pop_capture(const std::string& region_id)
{
    if (captures_.empty() || captures_.back().region_id != region_id)
    {
        return false;
    }
    captures_.pop_back();
    return true;
}

void InputArbiter::cancel_pointer_capture()
{
    button_capture_.reset();
    button_capture_button_ = MouseButton::none;
}

const Capture* InputArbiter::active_capture() const
{
    // The implicit button capture sits ABOVE the explicit stack: a drag started inside a dropdown
    // must keep reaching that dropdown even though the dropdown's own modal capture is below it.
    if (button_capture_.has_value())
    {
        return &button_capture_.value();
    }
    return captures_.empty() ? nullptr : &captures_.back();
}

void InputArbiter::fill_positions(PointerDispatch& out, const PointerEvent& event,
                                  const ShellRegion* region) const
{
    out.logical_position = to_logical_point(event.position, dpi_);
    if (region == nullptr)
    {
        out.region_position = PointI{};
        return;
    }
    out.region_position =
        PointI{event.position.x - static_cast<std::int32_t>(region->rect.origin.x),
               event.position.y - static_cast<std::int32_t>(region->rect.origin.y)};
}

PointerDispatch InputArbiter::route_pointer(const PointerEvent& event, std::uint64_t now_us)
{
    PointerDispatch dispatch;
    dispatch.dispatch_timestamp_us = now_us;

    // A press starts an implicit capture so a drag keeps reaching where it began, even once the
    // pointer has left that region. Resolved BEFORE the capture lookup below, so the press itself is
    // already routed by the capture it establishes.
    if (event.action == PointerAction::down && !button_capture_.has_value())
    {
        const ShellRegion* hit = regions_.hit_test(event.position);
        Capture capture;
        if (hit != nullptr)
        {
            capture.region_id = hit->id;
            capture.target = target_for(hit->kind);
        }
        else
        {
            // A press on browser chrome captures to the browser: CEF tracks its own drag, and the
            // pointer crossing a viewport rect mid-drag must not silently hand the stream over.
            capture.target = InputTarget::browser;
        }
        capture.modal = false;
        button_capture_ = capture;
        button_capture_button_ = event.button;
    }

    const Capture* capture = active_capture();
    if (capture != nullptr)
    {
        const ShellRegion* region =
            capture->region_id.empty() ? nullptr : regions_.find(capture->region_id);
        const bool inside = region != nullptr && contains(region->rect, event.position);

        // A modal capture the pointer is OUTSIDE of swallows the sample (the dropdown backdrop);
        // a non-modal (overlay) capture falls through to normal arbitration instead.
        if (!inside && capture->modal)
        {
            dispatch.target = InputTarget::swallowed;
            ++swallowed_;
            return dispatch;
        }
        if (inside || capture->target == InputTarget::browser || button_capture_.has_value())
        {
            dispatch.target = capture->target;
            dispatch.region_id = capture->region_id;
            fill_positions(dispatch, event, region);
            ++pointer_dispatches_;
            if (event.action == PointerAction::up && button_capture_.has_value() &&
                event.button == button_capture_button_)
            {
                button_capture_.reset();
                button_capture_button_ = MouseButton::none;
            }
            return dispatch;
        }
    }

    // No capture claimed it: plain region arbitration (§6.2).
    const ShellRegion* hit = regions_.hit_test(event.position);
    if (hit != nullptr)
    {
        dispatch.target = target_for(hit->kind);
        dispatch.region_id = hit->id;
    }
    else
    {
        dispatch.target = InputTarget::browser;
    }
    fill_positions(dispatch, event, hit);
    ++pointer_dispatches_;
    return dispatch;
}

KeyDispatch InputArbiter::route_key(const KeyEvent& event, std::uint64_t now_us)
{
    KeyDispatch dispatch;
    dispatch.dispatch_timestamp_us = now_us;
    ++key_dispatches_;

    // A DOM editable owns the keyboard outright — including the accelerators the keymap would
    // otherwise claim, because swallowing a key the user is typing into a text field is the one
    // failure this rule exists to prevent.
    if (focus_ == FocusClass::dom_editable)
    {
        dispatch.target = InputTarget::browser;
        return dispatch;
    }

    // A CHAR event is the text-producing half of a keystroke whose RAWKEYDOWN was already offered to
    // the keymap. Offering it again would let a keymap that claimed nothing still see two chances at
    // one physical key, so it goes straight through.
    if (event.action == KeyAction::character)
    {
        dispatch.target = InputTarget::browser;
        return dispatch;
    }

    if (keymap_resolver_ != nullptr && keymap_resolver_(event, keymap_user_data_))
    {
        dispatch.target = InputTarget::keymap;
        return dispatch;
    }

    // Unresolved keys fall through to the browser (§6.4) — today, that is all of them.
    dispatch.target = InputTarget::browser;
    return dispatch;
}

} // namespace context::editor::shell
