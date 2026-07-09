// Offscreen render + pixel-readback proof for the T1 native wgpu-native backend (R-REND-001/002).
// The production analog of the throwaway spikes/webgpu spike's self-check, but driven ENTIRELY
// through the rhi.h abstraction (render_offscreen_triangle) — the exact same code the fake-backend
// ctest runs — so a green here proves the wgpu backend implements the RHI correctly end to end.
//
//   probe  — R-HEAD-002 seam: enumerate adapters / report absence WITHOUT creating a device. Exit 0.
//   render — offscreen triangle -> texture -> readback buffer -> pixel asserts. Exit 0 pass /
//            77 skip (no adapter) / 1 fail. (default)

#include "context/render/offscreen_scene.h"
#include "context/render/wgpu/wgpu_rhi.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace context::render;

int main(int argc, char** argv)
{
    const std::string mode = (argc > 1) ? argv[1] : "render";

    std::unique_ptr<IRhi> rhi = create_wgpu_rhi();
    if (rhi == nullptr)
    {
        std::printf("[render-wgpu] SKIP: no WebGPU instance (WebGPU unavailable)\n");
        return (mode == "probe") ? 0 : 77;
    }

    if (mode == "probe")
    {
        const AdapterProbe probe = rhi->probe();
        std::printf("[render-wgpu] probe: %zu adapter(s) enumerated (no GPU device created)%s%s\n",
                    probe.adapter_count, probe.primary_name.empty() ? "" : " — primary: ",
                    probe.primary_name.c_str());
        return 0;
    }

    if (mode == "render")
    {
        const AdapterProbe probe = rhi->probe();
        if (!probe.has_adapter)
        {
            std::printf("[render-wgpu] SKIP: no WebGPU adapter available\n");
            return 77;
        }
        std::unique_ptr<IDevice> device = rhi->create_device();
        if (device == nullptr)
        {
            std::fprintf(stderr, "[render-wgpu] FAIL: device creation failed despite an adapter\n");
            return 1;
        }
        const OffscreenResult result = render_offscreen_triangle(*device);
        return result == OffscreenResult::Pass ? 0 : 1;
    }

    std::fprintf(stderr, "usage: %s [probe|render]\n", argv[0]);
    return 2;
}
