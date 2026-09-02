// The live Scene-viewport feed (see viewport_feed.h for the c3 / D7 rationale).

#include "context/editor/shell/panels/viewport_feed.h"

#include <cstdint>
#include <cstdio>
#include <utility>

namespace context::editor::shell::panels
{

std::string pick_selection_id(kernel::Entity entity)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "entity:%u:%u", static_cast<unsigned>(entity.index),
                  static_cast<unsigned>(entity.generation));
    return std::string(buf);
}

render::View framed_scene_view(const render::View& current)
{
    // The default framing pose comes from the PRODUCER (`default_scene_view`, viewport_binding.h),
    // not from a second set of literals here. That is what actually makes "frame scene" and "a
    // viewport nobody has ever moved" look at the same place — this file previously asserted the
    // property in a comment while spelling the pose independently, so the two were free to drift.
    const render::View defaults = default_scene_view();
    render::View framed = current;
    framed.transform.position[0] = defaults.transform.position[0];
    framed.transform.position[1] = defaults.transform.position[1];
    framed.transform.position[2] = defaults.transform.position[2];
    framed.transform.rotation[0] = 0.0f;
    framed.transform.rotation[1] = 0.0f;
    framed.transform.rotation[2] = 0.0f;
    framed.transform.rotation[3] = 1.0f;
    // The PROJECTION is the human's choice and survives (see the header): a 2D viewport that
    // re-framed itself into a perspective one would be answering a different question.
    return framed;
}

ViewportFeed::ViewportFeed(PanelHost& host, std::string panel_id, ViewportBinding* binding)
    : host_(host), panel_id_(std::move(panel_id)), binding_(binding)
{
    // NO PRODUCER IS NO ADAPTER (R-HEAD-002), which is also exactly what `bind_binding(nullptr)`
    // records — a feed constructed without a binding must not claim a rendering adapter it would
    // then report absent the moment the same nullptr arrived through the setter instead.
    adapter_available_ = binding_ != nullptr && binding_->adapter_available();
}

void ViewportFeed::bind_binding(ViewportBinding* binding)
{
    if (binding_ == binding)
    {
        // Called every frame (see the header): an unchanged producer must not touch the kind, or the
        // renderer would re-fetch every viewport's tree once per loop iteration forever. A null
        // producer has no verdict to re-read, so the current one simply stands — spelled as a guard
        // rather than by handing the field back to its own setter.
        if (binding != nullptr)
        {
            set_adapter_available(binding->adapter_available());
            // The composited size moves under an UNCHANGED producer on every dock resize, and this
            // is the only per-pump call the feed gets — so the re-read rides here rather than
            // needing a second per-frame hook in the composition root.
            sync_sizes();
        }
        return;
    }
    binding_ = binding;
    // A NEW producer holds no camera for any live copy, so the daemon read is re-armed: the copies
    // are still open and their cameras are still the human's, they just have to be hydrated again.
    fetch_due_ = true;
    set_adapter_available(binding == nullptr ? false : binding->adapter_available());
    // A new producer also publishes a DIFFERENT layer set (a detached one publishes none), and
    // `set_adapter_available` only refreshes when the VERDICT moved — so a producer swap that keeps
    // the verdict would otherwise leave every copy reporting the old producer's size.
    sync_sizes();
}

viewport::ViewportPanel* ViewportFeed::model(const std::string& instance_id)
{
    if (instance_id.empty())
    {
        // The host's bind-time PROBE (panel_host.h § provide_factory) calls the factory with an
        // empty id and DISCARDS the provider. Materialising a model for it would leave a permanent
        // phantom copy in `instances()` that no renderer ever opened.
        return nullptr;
    }
    auto it = models_.find(instance_id);
    if (it == models_.end())
    {
        it = models_.emplace(instance_id, viewport::ViewportPanel{}).first;
        refresh_present(instance_id, it->second);
        // A NEW COPY may be one the daemon already holds a camera for (a restored arrangement, a
        // rehomed panel), so re-arm the hydration read rather than assuming boot covered it.
        fetch_due_ = true;
    }
    return &it->second;
}

render::Extent2D ViewportFeed::composited_size(const std::string& instance_id) const
{
    if (binding_ != nullptr)
    {
        for (const ViewportLayer& layer : binding_->layers())
        {
            if (layer.id == instance_id)
            {
                return layer.content_size;
            }
        }
    }
    // No producer, or no layer published for this copy yet — the honest 0x0 the header names, which
    // `compute_present` reads as "nothing composited here", not as a render failure.
    return render::Extent2D{};
}

void ViewportFeed::refresh_present(const std::string& instance_id,
                                   viewport::ViewportPanel& panel) const
{
    const render::Extent2D size = composited_size(instance_id);
    // `scene_render_ok` is true: with no adapter the FIRST rule in `compute_present` already answers
    // `viewport.adapter_absent`, and claiming a render failure on top of it would report the wrong
    // one of two reserved codes for the same state.
    panel.set_present_env(gui_compositor::current_platform(), caps_, adapter_available_,
                          /*scene_render_ok*/ true, size.width, size.height);
}

void ViewportFeed::sync_sizes()
{
    bool changed = false;
    for (auto& [instance_id, panel] : models_)
    {
        const render::Extent2D size = composited_size(instance_id);
        // Compared against what the MODEL currently reports, not against a shadow copy kept here: a
        // second record of the same number is a second thing to keep in step, and the model already
        // holds the value the human reads. `present()` is exactly what `status_text`/`surface_text`
        // render from, so "the report is stale" and "these differ" are the same predicate.
        const viewport::ViewportPresent& present = panel.present();
        if (present.width == size.width && present.height == size.height)
        {
            continue;
        }
        refresh_present(instance_id, panel);
        changed = true;
    }
    if (changed)
    {
        // ONE touch for the whole pass, not one per copy: `touch` invalidates the panel KIND, so N
        // touches would cost N re-fetches of the same tree for a single frame's worth of change.
        host_.touch(panel_id_);
    }
}

void ViewportFeed::set_adapter_available(bool available)
{
    if (adapter_available_ == available)
    {
        return; // an unchanged verdict is not a model change and must not touch the kind
    }
    adapter_available_ = available;
    for (auto& [instance_id, panel] : models_)
    {
        refresh_present(instance_id, panel);
    }
    host_.touch(panel_id_);
}

std::size_t ViewportFeed::apply_cameras_result(const contract::Json& reply)
{
    if (binding_ == nullptr)
    {
        return 0;
    }
    const std::size_t adopted = binding_->apply_cameras_result(reply);
    cameras_applied_ += adopted;
    if (adopted > 0)
    {
        // The camera is not IN the rendered tree, but the status line's view generation is, and the
        // next composited frame is drawn from the adopted camera — so the kind is touched once per
        // adopted batch rather than per camera.
        host_.touch(panel_id_);
    }
    return adopted;
}

std::vector<contract::Json> ViewportFeed::take_camera_writes()
{
    std::vector<contract::Json> writes;
    if (binding_ == nullptr)
    {
        return writes;
    }
    const std::vector<std::string> dirty = binding_->take_dirty();
    writes.reserve(dirty.size());
    for (const std::string& viewport_id : dirty)
    {
        const render::View* view = binding_->find_camera(viewport_id);
        if (view == nullptr)
        {
            continue; // the copy closed between the move and the pump — nothing to persist
        }
        writes.push_back(camera_set_params(viewport_id, *view));
    }
    return writes;
}

void ViewportFeed::rearm_camera_write(const std::string& viewport_id)
{
    if (binding_ == nullptr || viewport_id.empty())
    {
        return;
    }
    // Re-armed through the binding's OWN dirty bookkeeping rather than a second queue here, so
    // there is exactly one answer to "which cameras owe the daemon a write".
    binding_->mark_camera_dirty(viewport_id);
}

bool ViewportFeed::pick(const std::string& instance_id, render::RegionPoint point,
                        render::Extent2D region_size, const render::RenderSnapshot& snapshot)
{
    if (scene_tree_ == nullptr)
    {
        return false; // nothing to drive — the same honest no-op every other write seam reports
    }
    // The copy's OWN camera, minting the Scene default on first sight — the SAME view the producer
    // renders with (viewport_binding.h's `camera()`), never a fixed transform: this is what makes a
    // moved camera pick a different entity.
    const render::View view =
        binding_ != nullptr ? binding_->camera(instance_id) : default_scene_view();
    const render::Ray ray = render::pick_ray(view, point, region_size);
    const render::PickHit hit = render::pick_nearest(ray, snapshot);
    return hit.hit ? scene_tree_->select(pick_selection_id(hit.entity))
                   : scene_tree_->clear_selection();
}

PanelProviderFactory ViewportFeed::make_factory()
{
    return [this](const std::string& instance_id) -> PanelProvider
    {
        PanelProvider provider;
        provider.build = [this, instance_id]() -> gui::uitree::Panel
        {
            viewport::ViewportPanel* panel = model(instance_id);
            if (panel == nullptr)
            {
                // The probe's provider: it is discarded before anything renders, but `build` must be
                // non-null for the binding to be accepted at all, and it must be safe to call.
                return viewport::ViewportPanel{}.build_panel();
            }
            // Re-read the present environment on every build: the composited SIZE is the producer's
            // and moves whenever the panel is resized, so a value cached at construction would make
            // the status line report the size the panel had when it opened.
            refresh_present(instance_id, *panel);
            return panel->build_panel();
        };
        provider.invoke = [this, instance_id](const std::string& command_id,
                                              const contract::Json&) -> bool
        {
            if (command_id != viewport::kFrameSceneCommand || instance_id.empty())
            {
                return false;
            }
            viewport::ViewportPanel* panel = model(instance_id);
            if (panel == nullptr)
            {
                return false;
            }
            ++frame_scene_requests_;
            // The OBSERVER half: advance the view generation + notify listeners (R-HUX-011's
            // gesture->viewport-update loop, which the panel already owns).
            panel->frame_scene();
            // The CAMERA half (e3): the same affordance is the one thing that moves a camera today,
            // so it writes through the binding — which arms the `editor.camera-set` the pump sends.
            if (binding_ != nullptr)
            {
                (void)binding_->set_camera(instance_id,
                                           framed_scene_view(binding_->camera(instance_id)));
            }
            host_.touch(panel_id_);
            return true;
        };
        // NO get_state / restore_state, deliberately: the camera persists where cameras persist
        // (the daemon), and a D6 blob carrying it would be a second source of truth. See the header.
        return provider;
    };
}

} // namespace context::editor::shell::panels
