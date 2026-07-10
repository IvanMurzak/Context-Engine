// Offscreen render + pixel-readback proof for the T1 native wgpu-native backend (R-REND-001/002).
// The production analog of the throwaway spikes/webgpu spike's self-check, but driven ENTIRELY
// through the rhi.h abstraction (render_offscreen_triangle) — the exact same code the fake-backend
// ctest runs — so a green here proves the wgpu backend implements the RHI correctly end to end.
//
//   probe  — R-HEAD-002 seam: enumerate adapters / report absence WITHOUT creating a device. Exit 0.
//   render — offscreen triangle -> texture -> readback buffer -> pixel asserts. Exit 0 pass /
//            77 skip (no adapter) / 1 fail. (default)
//   sprite — R-2D-001 GPU sprite-draw proof: ortho-projected quads + sorting-layer overdraw ->
//            texture -> readback -> pixel asserts. Same exit convention as `render`.
//   lit    — R-REND-004/006 GPU PBR proof: World -> extract -> shadow depth pass + lit main pass ->
//            readback asserts vs the CPU reference (lighting/shadow/lightmap-hook deltas). Same
//            exit convention as `render`.

#include "context/render/lit/lit_offscreen.h"
#include "context/render/offscreen_scene.h"
#include "context/render/sprite/sprite_offscreen.h"
#include "context/render/wgpu/wgpu_rhi.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace context::render;

namespace
{

// Exit past wgpu-native's flaky Windows teardown once the asserts are done.
//
// On the self-hosted Windows CI runners (LocalSystem services in Session 0) wgpu-native's
// instance/device TEARDOWN — wgpuInstanceRelease plus the Vulkan/DX backend worker-thread join it
// triggers — intermittently __fastfails with 0xc0000409 (STATUS_STACK_BUFFER_OVERRUN). It is a
// NONDETERMINISTIC native-library teardown race (the same commit passed then failed on rerun),
// unrelated to our RHI logic: the `probe` path does no buffer/readback work at all, only instance
// create + adapter enumerate + release. This is exactly why the `render-wgpu-offscreen` ctest is
// deliberately unregistered on Windows (see src/render/CMakeLists.txt) — the `probe` test hit the
// same race. Since the process is exiting anyway and every assertion has already run, on Windows we
// flush stdio (so the [render-wgpu] log lines survive) and std::_Exit past the RAII destructors, so
// the flaky wgpu teardown never runs. POSIX unwinds normally (clean and validated on the lavapipe
// leg), keeping the real wgpuInstanceRelease/device-release coverage there — so this is a no-op
// difference off Windows.
void finish(int code)
{
    std::fflush(stdout);
    std::fflush(stderr);
#if defined(_WIN32)
    std::_Exit(code);
#else
    (void)code; // POSIX: fall through to a normal return so the RAII teardown path is exercised.
#endif
}

// Probe + create a device for the readback modes. Returns the device, or nullptr with *exit_code set
// to the process exit code (77 = no adapter -> SKIP, 1 = device creation failed -> FAIL). Shared by
// the `render` and `sprite` modes, which differ only in which proof they then run.
std::unique_ptr<IDevice> acquire_device(IRhi& rhi, int& exit_code)
{
    const AdapterProbe probe = rhi.probe();
    if (!probe.has_adapter)
    {
        std::printf("[render-wgpu] SKIP: no WebGPU adapter available\n");
        exit_code = 77;
        return nullptr;
    }
    std::unique_ptr<IDevice> device = rhi.create_device();
    if (device == nullptr)
    {
        std::fprintf(stderr, "[render-wgpu] FAIL: device creation failed despite an adapter\n");
        exit_code = 1;
    }
    return device;
}

} // namespace

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
        finish(0);
        return 0;
    }

    if (mode == "render" || mode == "sprite" || mode == "lit")
    {
        int exit_code = 0;
        std::unique_ptr<IDevice> device = acquire_device(*rhi, exit_code);
        if (device == nullptr)
        {
            finish(exit_code);
            return exit_code;
        }
        bool pass = false;
        if (mode == "render")
        {
            pass = render_offscreen_triangle(*device) == OffscreenResult::Pass;
        }
        else if (mode == "sprite")
        {
            pass = context::render::sprite::render_offscreen_sprite(*device);
        }
        else
        {
            pass = context::render::lit::render_offscreen_lit(*device);
        }
        const int code = pass ? 0 : 1;
        finish(code);
        return code;
    }

    std::fprintf(stderr, "usage: %s [probe|render|sprite|lit]\n", argv[0]);
    return 2;
}
