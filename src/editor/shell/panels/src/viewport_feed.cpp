// The live Scene-viewport feed (see viewport_feed.h for the c3 / D7 rationale).

#include "context/editor/shell/panels/viewport_feed.h"

#include <utility>

namespace context::editor::shell::panels
{

namespace
{

// The default framing pose, shared by `framed_scene_view` and the binding's own first-sight camera
// (viewport_binding.h). Spelled once here so "frame scene" and "a viewport nobody has ever moved"
// cannot end up looking at two different places.
constexpr float kDefaultEyeY = 3.0f;
constexpr float kDefaultEyeZ = 8.0f;

} // namespace

render::View framed_scene_view(const render::View& current)
{
    render::View framed = current;
    framed.transform.position[0] = 0.0f;
    framed.transform.position[1] = kDefaultEyeY;
    framed.transform.position[2] = kDefaultEyeZ;
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
    if (binding_ != nullptr)
    {
        adapter_available_ = binding_->adapter_available();
    }
}

void ViewportFeed::bind_binding(ViewportBinding* binding)
{
    if (binding_ == binding)
    {
        // Called every frame (see the header): an unchanged producer must not touch the kind, or the
        // renderer would re-fetch every viewport's tree once per loop iteration forever.
        set_adapter_available(binding == nullptr ? adapter_available_ : binding->adapter_available());
        return;
    }
    binding_ = binding;
    // A NEW producer holds no camera for any live copy, so the daemon read is re-armed: the copies
    // are still open and their cameras are still the human's, they just have to be hydrated again.
    fetch_due_ = true;
    set_adapter_available(binding == nullptr ? false : binding->adapter_available());
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

void ViewportFeed::refresh_present(const std::string& instance_id,
                                   viewport::ViewportPanel& panel) const
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (binding_ != nullptr)
    {
        for (const ViewportLayer& layer : binding_->layers())
        {
            if (layer.id == instance_id)
            {
                width = layer.content_size.width;
                height = layer.content_size.height;
                break;
            }
        }
    }
    // `scene_render_ok` is true: with no adapter the FIRST rule in `compute_present` already answers
    // `viewport.adapter_absent`, and claiming a render failure on top of it would report the wrong
    // one of two reserved codes for the same state.
    panel.set_present_env(gui_compositor::current_platform(), caps_, adapter_available_,
                          /*scene_render_ok*/ true, width, height);
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
