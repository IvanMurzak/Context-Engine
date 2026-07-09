// The optional, detachable renderer (R-HEAD-002) — see renderer.h.

#include "context/render/renderer.h"

namespace context::render
{

std::unique_ptr<Renderer> Renderer::try_attach(IRhi& rhi)
{
    // R-HEAD-002: probe WITHOUT creating a device first. No adapter => stay headless (no renderer).
    const AdapterProbe probe = rhi.probe();
    if (!probe.has_adapter)
    {
        return nullptr;
    }

    std::unique_ptr<IDevice> device = rhi.create_device();
    if (device == nullptr)
    {
        // Adapter reported but device creation failed — still no renderer, still no render work.
        return nullptr;
    }

    // Private ctor: use `new` because make_unique cannot reach it.
    return std::unique_ptr<Renderer>(new Renderer(std::move(device), rhi.backend_name()));
}

Renderer::Renderer(std::unique_ptr<IDevice> device, const char* backend_name)
    : device_(std::move(device)), backend_name_(backend_name)
{
}

Renderer::~Renderer() = default;

void Renderer::render(const RenderSnapshot& snapshot)
{
    // A foundation frame: consume the snapshot. The concrete draw (pipeline + per-item draw calls
    // over the extracted transforms) is exercised end-to-end by the offscreen readback proof in the
    // wgpu backend; here the facade records that a frame was rendered from the snapshot so headless
    // (no Renderer) vs rendered (a Renderer consuming frames) is observable without a GPU.
    stats_.frames += 1;
    stats_.last_item_count = snapshot.items.size();
    stats_.last_sim_tick = snapshot.sim_tick;
}

} // namespace context::render
