// The Scene viewport binding seam (see viewport_binding.h for the three-ids map and the D7 rationale).

#include "context/editor/shell/viewport_binding.h"

#include "json_number_read.h"

#include "context/render/viewport_pass.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace context::editor::shell
{

namespace
{

// The float range as doubles, for the guard below. A camera blob is UNTRUSTED — it round-trips
// through a hand-editable `.editor/session.json` and `Json::parse` accepts `1e300` happily — and
// `static_cast<float>` of a double outside float's range is UNDEFINED BEHAVIOUR, which the blocking
// `sanitize (ASan+UBSan, ubuntu)` leg reports as `float-cast-overflow`. So the range is checked on
// the DOUBLE, before the narrowing cast; guarding after would be guarding after the UB happened.
constexpr double kFloatMin = -3.4e38;
constexpr double kFloatMax = 3.4e38;

// Read one float member, leaving `out` alone when the member is absent, not a number, or outside
// float's range. Every read in this file is total for the reason the header states: a camera blob
// from an older build must cost the human a FIELD at worst, never their viewport.
//
// Delegated to the Shell's ONE range-guarded numeric reader (json_number_read.h) rather than
// re-derived, which is the mistake that header exists to stop repeating.
bool read_float(const contract::Json& object, const char* key, float& out)
{
    const std::optional<double> value =
        detail::number_in_range(object, key, kFloatMin, kFloatMax);
    if (!value.has_value())
    {
        return false;
    }
    out = static_cast<float>(*value);
    return true;
}

// Read a fixed-length float array (`[x, y, z]`). Partial arrays are REFUSED wholesale rather than
// applied element-wise: half a position is a camera somewhere nobody asked for, which is worse than
// the one it already had. `Json::at` is total (it answers the shared null for a non-object), so no
// `is_object()` guard is needed ahead of it.
bool read_float_array(const contract::Json& object, const char* key, float* out, std::size_t count)
{
    const contract::Json& value = object.at(key);
    if (!value.is_array() || value.size() != count)
    {
        return false;
    }
    // The same guard `number_in_range` applies, per ELEMENT — it reads members by key and these
    // arrive by index, so the check is spelled here rather than skipped. A NaN fails both
    // comparisons and is refused with everything else out of range.
    for (std::size_t i = 0; i < count; ++i)
    {
        const contract::Json& element = value.at(i);
        if (!element.is_number())
        {
            return false;
        }
        const double raw = element.as_number();
        if (!(raw >= kFloatMin && raw <= kFloatMax))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        out[i] = static_cast<float>(value.at(i).as_number());
    }
    return true;
}

contract::Json float_array(const float* values, std::size_t count)
{
    contract::Json out = contract::Json::array();
    for (std::size_t i = 0; i < count; ++i)
    {
        out.push_back(contract::Json(static_cast<double>(values[i])));
    }
    return out;
}

// The mode/type tokens. Spelled here and nowhere else — they are OUR vocabulary inside an opaque
// blob, so the daemon never sees them and no mirror site can drift from them.
constexpr const char* kMode2d = "2d";
constexpr const char* kMode3d = "3d";
constexpr const char* kTypeScene = "scene";
constexpr const char* kTypeGame = "game";

// Every field of a View that the codec round-trips. Compared rather than `memcmp`ed because View
// holds floats and padding, and a padding byte differing would report a camera move that is not one.
[[nodiscard]] bool same_view(const render::View& a, const render::View& b)
{
    for (int i = 0; i < 3; ++i)
    {
        if (a.transform.position[i] != b.transform.position[i] ||
            a.transform.scale[i] != b.transform.scale[i])
        {
            return false;
        }
    }
    for (int i = 0; i < 4; ++i)
    {
        if (a.transform.rotation[i] != b.transform.rotation[i])
        {
            return false;
        }
    }
    return a.projection.fov_y_radians == b.projection.fov_y_radians &&
           a.projection.ortho_half_height == b.projection.ortho_half_height &&
           a.projection.near_z == b.projection.near_z &&
           a.projection.far_z == b.projection.far_z && a.mode == b.mode && a.type == b.type;
}

[[nodiscard]] bool same_rect(const render::Rect2D& a, const render::Rect2D& b)
{
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.size.width == b.size.width &&
           a.size.height == b.size.height;
}

// The `cameras` array of an `editor.cameras-get` reply, wherever it sits in the envelope. Mirrors
// FilesFeed::apply_result's tolerance: the envelope, the bare data, or the bare object.
[[nodiscard]] const contract::Json& cameras_array(const contract::Json& reply)
{
    // `Json::at` is total, so the nested probe is safe on any shape and the fall-through answers the
    // shared null — `is_array()` is false there and the caller stops.
    const contract::Json& direct = reply.at("cameras");
    return direct.is_array() ? direct : reply.at("data").at("cameras");
}

} // namespace

// --------------------------------------------------------------------------- the camera codec

contract::Json camera_transform_json(const render::View& view)
{
    contract::Json out = contract::Json::object();
    out.set("position", float_array(view.transform.position, 3));
    out.set("rotation", float_array(view.transform.rotation, 4));
    out.set("scale", float_array(view.transform.scale, 3));
    return out;
}

contract::Json camera_projection_json(const render::View& view)
{
    contract::Json out = contract::Json::object();
    out.set("mode", contract::Json(view.mode == render::ViewMode::two_d ? kMode2d : kMode3d));
    out.set("type", contract::Json(view.type == render::ViewType::game ? kTypeGame : kTypeScene));
    out.set("fovY", contract::Json(static_cast<double>(view.projection.fov_y_radians)));
    out.set("orthoHalfHeight",
            contract::Json(static_cast<double>(view.projection.ortho_half_height)));
    out.set("near", contract::Json(static_cast<double>(view.projection.near_z)));
    out.set("far", contract::Json(static_cast<double>(view.projection.far_z)));
    return out;
}

bool apply_camera_transform(render::View& view, const contract::Json& transform)
{
    bool read = false;
    read |= read_float_array(transform, "position", view.transform.position, 3);
    read |= read_float_array(transform, "rotation", view.transform.rotation, 4);
    read |= read_float_array(transform, "scale", view.transform.scale, 3);
    return read;
}

bool apply_camera_projection(render::View& view, const contract::Json& projection)
{
    bool read = false;
    // No `is_object()` guard: `Json::at` answers the shared null for a non-object, so `is_string()`
    // alone already decides it (the same totality `cameras_array` above relies on).
    if (projection.at("mode").is_string())
    {
        const std::string& mode = projection.at("mode").as_string();
        // An UNRECOGNIZED token keeps the current mode rather than defaulting to 3D: a 2D viewport
        // silently becoming perspective is a worse answer than one that ignores a word it does not
        // know.
        if (mode == kMode2d)
        {
            view.mode = render::ViewMode::two_d;
            read = true;
        }
        else if (mode == kMode3d)
        {
            view.mode = render::ViewMode::three_d;
            read = true;
        }
    }
    if (projection.at("type").is_string())
    {
        const std::string& type = projection.at("type").as_string();
        if (type == kTypeScene)
        {
            view.type = render::ViewType::scene;
            read = true;
        }
        else if (type == kTypeGame)
        {
            view.type = render::ViewType::game;
            read = true;
        }
    }
    read |= read_float(projection, "fovY", view.projection.fov_y_radians);
    read |= read_float(projection, "orthoHalfHeight", view.projection.ortho_half_height);
    read |= read_float(projection, "near", view.projection.near_z);
    read |= read_float(projection, "far", view.projection.far_z);
    return read;
}

contract::Json camera_set_params(const std::string& viewport_id, const render::View& view)
{
    contract::Json params = contract::Json::object();
    params.set("viewportId", contract::Json(viewport_id));
    params.set("transform", camera_transform_json(view));
    params.set("projection", camera_projection_json(view));
    return params;
}

// -------------------------------------------------------------------------------- the binding

void ViewportBinding::attach_device(render::IDevice& device)
{
    if (device_ == &device && targets_ != nullptr)
    {
        return; // the same device: keep every live target rather than churning the whole window
    }
    device_ = &device;
    // A target is device-bound, so a NEW device invalidates every one of them. Rebuilding the
    // registry is what drops them; the entries keep their cameras and are re-acquired on the next
    // publish (their slots are re-minted there too — a slot is registry-local).
    targets_ = std::make_unique<render::ViewportTargetRegistry>(device);
    // …and every layer ALREADY published names a view the line above just destroyed.
    invalidate_targets();
}

void ViewportBinding::detach_device()
{
    device_ = nullptr;
    targets_.reset();
    invalidate_targets();
}

void ViewportBinding::invalidate_targets()
{
    // The registry was replaced or destroyed, so every slot is void AND the compositor still holds
    // layers whose `content` views it owned. Arm the unconditional republish so none of those
    // dangling pointers survives to `render_gpu_frame` (see `needs_publish`) — without it the next
    // publish would compare an unchanged rect set and take the `mark_viewport_content()` branch,
    // leaving them in place.
    //
    // ONE spelling for both device transitions: the invariant is the same on each, and a third
    // caller (a device-loss path) inherits it rather than copying it a third time.
    force_publish_ = true;
    // NOT a structured binding named `entry`: that is a member function of this class, and a local
    // hiding a class member is MSVC C4458 — an ERROR under /W4 /WX, invisible to the local GCC gate.
    for (auto& live : entries_)
    {
        live.second.slot = 0;
    }
}

const char* ViewportBinding::degraded_code() const noexcept
{
    return device_ == nullptr ? kViewportAdapterAbsentCode : "";
}

render::View default_scene_view()
{
    render::View view{};
    // Pulled back and up, looking down the -Z axis the grid lies under (header note). The ONE
    // spelling: `framed_scene_view` reads its position from here rather than repeating it.
    view.transform.position[0] = 0.0f;
    view.transform.position[1] = 3.0f;
    view.transform.position[2] = 8.0f;
    view.mode = render::ViewMode::three_d;
    view.type = render::ViewType::scene;
    return view;
}

ViewportBinding::Entry& ViewportBinding::entry(const std::string& viewport_id)
{
    auto it = entries_.find(viewport_id);
    if (it != entries_.end())
    {
        return it->second;
    }
    Entry fresh;
    fresh.view = default_scene_view();
    return entries_.emplace(viewport_id, fresh).first->second;
}

render::View& ViewportBinding::camera(const std::string& viewport_id)
{
    return entry(viewport_id).view;
}

const render::View* ViewportBinding::find_camera(const std::string& viewport_id) const
{
    const auto it = entries_.find(viewport_id);
    return it == entries_.end() ? nullptr : &it->second.view;
}

bool ViewportBinding::apply_camera(const std::string& viewport_id, const contract::Json& transform,
                                   const contract::Json& projection)
{
    Entry& target = entry(viewport_id);
    const render::View before = target.view;
    (void)apply_camera_transform(target.view, transform);
    (void)apply_camera_projection(target.view, projection);
    // NOT marked dirty, deliberately: this camera came FROM the daemon, and pushing it back would be
    // this window echoing a fact to its own author — the `origin` echo-suppression posture
    // (docs/editor-session-state.md), applied at the one seam that can see both directions.
    return !same_view(before, target.view);
}

std::size_t ViewportBinding::apply_cameras_result(const contract::Json& reply)
{
    const contract::Json& cameras = cameras_array(reply);
    if (!cameras.is_array())
    {
        return 0;
    }
    std::size_t adopted = 0;
    for (std::size_t i = 0; i < cameras.size(); ++i)
    {
        const contract::Json& element = cameras.at(i);
        if (!element.is_object() || !element.at("viewportId").is_string())
        {
            continue; // an unparseable ENTRY is skipped, never fatal (the ProblemsFeed tolerance)
        }
        const std::string& id = element.at("viewportId").as_string();
        if (id.empty())
        {
            continue;
        }
        if (std::find(dirty_.begin(), dirty_.end(), id) != dirty_.end())
        {
            // A LOCAL move for this viewport is still on the write queue, so the daemon's copy is
            // by definition the OLDER one. Adopting it here would revert the human's gesture and —
            // worse — the very next `take_dirty()` would push the reverted value back as if it were
            // the move, making the loss permanent. The pending write wins; the daemon hears it, and
            // the next hydration reads what we wrote.
            continue;
        }
        (void)apply_camera(id, element.at("transform"), element.at("projection"));
        ++adopted;
    }
    return adopted;
}

bool ViewportBinding::set_camera(const std::string& viewport_id, const render::View& view)
{
    Entry& target = entry(viewport_id);
    if (same_view(target.view, view))
    {
        return false; // an unmoved camera is not a write — nothing is pushed to the daemon
    }
    const std::uint32_t slot = target.view.viewport_id;
    target.view = view;
    // The render slot is OURS, never the caller's: a View handed in from a gesture carries whatever
    // slot it was copied from (or 0), and adopting that would re-point this viewport at another
    // viewport's target.
    target.view.viewport_id = slot;
    if (std::find(dirty_.begin(), dirty_.end(), viewport_id) == dirty_.end())
    {
        dirty_.push_back(viewport_id);
    }
    return true;
}

void ViewportBinding::mark_camera_dirty(const std::string& viewport_id)
{
    if (entries_.find(viewport_id) == entries_.end())
    {
        return; // the copy closed; there is no camera left to persist
    }
    if (std::find(dirty_.begin(), dirty_.end(), viewport_id) == dirty_.end())
    {
        dirty_.push_back(viewport_id);
    }
}

std::vector<std::string> ViewportBinding::take_dirty()
{
    std::vector<std::string> out;
    out.swap(dirty_);
    return out;
}

std::uint32_t ViewportBinding::render_slot(const std::string& viewport_id) const
{
    const auto it = entries_.find(viewport_id);
    return it == entries_.end() ? 0u : it->second.slot;
}

std::size_t ViewportBinding::live_targets() const
{
    return targets_ == nullptr ? 0u : targets_->live_targets();
}

ViewportPublishStats ViewportBinding::publish(const std::vector<ShellRegion>& regions,
                                              const render::RenderSnapshot& snapshot,
                                              WindowCompositor& compositor)
{
    ViewportPublishStats stats;
    stats.adapter_absent = device_ == nullptr;
    ++publishes_;
    // A device change invalidated every published `content` view, so this publish must reach the
    // compositor whatever the rects say (viewport_binding.h § needs_publish).
    const bool forced = force_publish_;
    force_publish_ = false;

    for (auto& [id, live] : entries_)
    {
        (void)id;
        live.present = false;
    }

    std::vector<ViewportLayer> layers;
    layers.reserve(regions.size());
    for (const ShellRegion& region : regions)
    {
        if (region.kind != RegionKind::viewport || region.id.empty())
        {
            continue;
        }
        ++stats.viewports;
        Entry& live = entry(region.id);
        live.present = true;
        if (render::is_empty(region.rect.size))
        {
            // A DEGENERATE rect is a real transient (a panel mid-resize, a collapsed group), so it
            // is dropped from the layer stack rather than published as a zero-area hole — and the
            // target is deliberately KEPT at its last valid size, mirroring
            // ViewportTargetRegistry::resize's own no-op rule.
            if (!same_rect(live.rect, region.rect))
            {
                live.rect = region.rect;
                stats.changed = true;
            }
            continue;
        }
        if (!same_rect(live.rect, region.rect))
        {
            live.rect = region.rect;
            stats.changed = true;
        }

        ViewportLayer layer;
        layer.id = region.id;
        layer.content_rect = region.rect;
        layer.content_size = region.rect.size;

        if (targets_ != nullptr && device_ != nullptr)
        {
            if (live.slot == 0)
            {
                live.slot = next_slot_++;
                stats.changed = true; // a viewport that just appeared IS a layout change
            }
            live.view.viewport_id = live.slot;
            const render::ViewportTargetId target =
                targets_->acquire_for(live.slot, region.rect.size);
            if (target != render::kInvalidViewportTarget)
            {
                render::ViewportPassConfig config;
                config.depth = targets_->depth_view(target);
                // D5: a GAME view gets NO edit-time overlays, and deciding that from the View is
                // exactly this binding's job (viewport_pass.h says so in as many words).
                config.draw_grid = live.view.type == render::ViewType::scene;
                render::ITextureView* colour = targets_->color_view(target);
                if (colour != nullptr)
                {
                    const render::ViewportFrameStats frame = render::render_viewport_view(
                        *device_, live.view, snapshot, *colour, region.rect.size, config);
                    if (frame.rendered)
                    {
                        ++stats.rendered;
                    }
                    layer.content = colour;
                }
            }
        }

        layers.push_back(std::move(layer));
        ++stats.layers;
    }

    // Release the targets of viewports whose region went away — the half that makes a closed
    // viewport give its GPU memory back rather than merely stop being drawn.
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (it->second.present)
        {
            ++it;
            continue;
        }
        if (targets_ != nullptr && it->second.slot != 0)
        {
            (void)targets_->release_for(it->second.slot);
        }
        // The ENTRY goes too, camera and all. That is not a loss: the daemon is the camera's
        // custodian (`.editor/session.json`), so a viewport reopened under the same instance id
        // hydrates its camera back through `editor.cameras-get`. Keeping a per-window shadow copy
        // alive instead would be a second source of truth free to disagree with the persisted one.
        it = entries_.erase(it);
        stats.changed = true;
    }

    // Only when the answer is not already known: a rect that moved, a viewport that appeared or one
    // that closed has already decided this, and the id-string compare below cannot change it. Those
    // are exactly the publishes a drag produces, which is when this runs at all.
    if (!stats.changed)
    {
        if (layers.size() != layers_.size())
        {
            stats.changed = true;
        }
        else
        {
            for (std::size_t i = 0; i < layers.size(); ++i)
            {
                if (layers[i].id != layers_[i].id ||
                    !same_rect(layers[i].content_rect, layers_[i].content_rect))
                {
                    stats.changed = true;
                    break;
                }
            }
        }
    }

    if (stats.changed || forced)
    {
        ++layout_changes_;
        // A moved / added / removed rect IS a layout change, and `publish_viewports` sets exactly
        // that damage flag itself (compositor.h § Damage). Passed by lvalue: the parameter is
        // by-value, so this makes the ONE deep copy that is genuinely needed, and `layers` is then
        // MOVED into `layers_` below instead of being copied there as well.
        compositor.publish_viewports(layers);
    }
    else
    {
        // The SAME layer stack, redrawn. Republishing it would set `layout` damage for a layout that
        // did not change — the flags are a diagnostic record of WHY a frame was drawn (compositor.h),
        // so a redraw filed under the wrong reason is the exact thing they exist to prevent. The
        // published `ITextureView*`s still point at the targets just drawn into (an unchanged rect
        // is a no-op in `acquire_for`, so nothing was reallocated), which is what makes NOT
        // republishing correct rather than merely cheaper.
        //
        // This is the FIRST caller of `mark_viewport_content()` — the seam compositor.h reserved for
        // "a viewport's CONTENT changed without its rect changing" and that nothing had reached.
        //
        // Only when there IS a viewport: with no viewport layers at all nothing was redrawn, and
        // damaging the frame anyway would make every chrome-only region republish present a frame
        // for a viewport that does not exist.
        if (!layers.empty())
        {
            compositor.mark_viewport_content();
        }
    }
    // Adopted LAST, and by move: on the republish path the compositor already took its own copy
    // above, and on the redraw path nothing needs a copy at all. (Reading `layers` rather than
    // `layers_` in the branch above is the same question either way — `stats.changed` is false
    // there, which is exactly the case where the two stacks have equal size.)
    layers_ = std::move(layers);
    return stats;
}

} // namespace context::editor::shell
