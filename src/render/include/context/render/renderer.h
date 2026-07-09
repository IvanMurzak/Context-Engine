// The renderer — the optional, detachable rendering subsystem (R-HEAD-002, R-KERNEL-002).
//
// Rendering is a package composed on top of the kernel, never a kernel dependency. The engine runs
// with this module absent entirely. R-HEAD-002: when headless, NO GPU device is created and NO
// rendering work is done; when non-headless, a GPU device is required and drawing happens normally.
//
// The detach contract is structural: a Renderer OWNS an IDevice, so a Renderer only exists when a
// device was created. `try_attach()` probes for an adapter and returns nullptr when none exists (a
// GPU-less / headless box) — the caller then simply has no Renderer and does no render work. Because
// the Renderer holds the device, "headless" is the absence of a Renderer, not a runtime flag on one.

#pragma once

#include "context/render/render_world.h"
#include "context/render/rhi.h"

#include <memory>

namespace context::render
{

// The number of drawables the last render() observed — a trivial, GPU-free bit of evidence that
// render consumed the snapshot, used by tests and by headless-vs-rendered assertions.
struct RenderStats
{
    std::uint64_t frames = 0;
    std::size_t last_item_count = 0;
    std::uint64_t last_sim_tick = 0;
};

class Renderer
{
public:
    // Attach rendering to `rhi`: probe for an adapter and, if present, create a device. Returns
    // nullptr when no adapter exists — the R-HEAD-002 headless path (no device, no renderer). The
    // caller keeps `rhi` alive for the Renderer's lifetime.
    [[nodiscard]] static std::unique_ptr<Renderer> try_attach(IRhi& rhi);

    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Draw one frame from a render snapshot (the L-39 front buffer). Read-only over the snapshot.
    void render(const RenderSnapshot& snapshot);

    [[nodiscard]] const RenderStats& stats() const { return stats_; }
    [[nodiscard]] IDevice& device() { return *device_; }
    [[nodiscard]] const char* backend_name() const { return backend_name_; }

private:
    Renderer(std::unique_ptr<IDevice> device, const char* backend_name);

    std::unique_ptr<IDevice> device_;
    const char* backend_name_ = "unknown";
    RenderStats stats_;
};

} // namespace context::render
