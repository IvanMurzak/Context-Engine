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
//   viewport — M5-F1 (issue #164): the observer viewport's "live scene (3D+2D)" composite — the 3D
//            triangle base with the 2D sprites overlaid — read back + asserted (clear + 3D layer +
//            2D-on-top). Same exit convention as `render`.
//   golden <scene> <out.ppm>
//          — M4 T7 (issue #141): render a golden-corpus scene (triangle3d | sprite2d | lit3d) and
//            write the frame as binary PPM for the SSIM visual-equivalence gate
//            (tools/golden_compare.py vs the committed baseline under goldens/). Same exit
//            convention as `render`; exit 2 on a bad scene id / unwritable path.
//   bench [frames] [warmup] [WxH]
//          — M4 T7 (issue #141): the R-QA-007 min-spec floor bench subject — the representative
//            lit scene rendered frame-after-frame offscreen (defaults 60 frames, 10 warmup,
//            1920x1080, the committed floor resolution). Prints one JSON line of raw per-frame
//            samples; bench/minspec_floor.py owns the R-QA-009 median-of-5/band policy. Same exit
//            convention as `render`; exit 2 on bad arguments.

#include "context/render/golden.h"
#include "context/render/lit/golden_lit.h"
#include "context/render/lit/lit_offscreen.h"
#include "context/render/offscreen_scene.h"
#include "context/render/sprite/sprite_offscreen.h"
#include "context/render/viewport_scene.h"
#include "context/render/wgpu/wgpu_rhi.h"

#include <cstdint>
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

    if (mode == "render" || mode == "sprite" || mode == "lit" || mode == "viewport")
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
        else if (mode == "viewport")
        {
            pass = render_offscreen_viewport(*device); // M5-F1: the 3D+2D observer composite (#164)
        }
        else
        {
            pass = context::render::lit::render_offscreen_lit(*device);
        }
        const int code = pass ? 0 : 1;
        finish(code);
        return code;
    }

    if (mode == "golden")
    {
        if (argc < 4)
        {
            std::fprintf(stderr, "usage: %s golden <triangle3d|sprite2d|lit3d|viewport> <out.ppm>\n",
                         argv[0]);
            return 2;
        }
        const std::string scene = argv[2];
        const std::string out_path = argv[3];

        int exit_code = 0;
        std::unique_ptr<IDevice> device = acquire_device(*rhi, exit_code);
        if (device == nullptr)
        {
            finish(exit_code);
            return exit_code;
        }
        golden::GoldenImage image;
        bool rendered = false;
        if (scene == "lit3d")
        {
            rendered = lit::render_golden_lit3d(*device, image);
        }
        else if (scene == "viewport")
        {
            rendered = render_golden_viewport(*device, image); // M5-F1 observer composite (#164)
        }
        else if (scene == "triangle3d" || scene == "sprite2d")
        {
            rendered = golden::render_golden_scene(*device, scene, image);
        }
        else
        {
            std::fprintf(stderr, "[render-golden] unknown scene '%s'\n", scene.c_str());
            finish(2);
            return 2;
        }
        if (!rendered)
        {
            std::fprintf(stderr, "[render-golden] FAIL: could not render scene '%s'\n",
                         scene.c_str());
            finish(1);
            return 1;
        }
        if (!golden::write_ppm(image, out_path))
        {
            std::fprintf(stderr, "[render-golden] FAIL: could not write '%s'\n", out_path.c_str());
            finish(2);
            return 2;
        }
        std::printf("[render-golden] scene=%s -> %s (%ux%u)\n", scene.c_str(), out_path.c_str(),
                    image.width, image.height);
        finish(0);
        return 0;
    }

    if (mode == "bench")
    {
        std::uint32_t frames = 60;
        std::uint32_t warmup = 10;
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        if (argc > 2)
        {
            frames = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
        }
        if (argc > 3)
        {
            warmup = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10));
        }
        if (argc > 4)
        {
            // Parse "WxH" without sscanf (raw C stdio trips MSVC /W4 /WX C4996 — conventions.md).
            const std::string res = argv[4];
            const std::size_t sep = res.find('x');
            char* rest = nullptr;
            const unsigned long parsed_w =
                (sep == std::string::npos) ? 0 : std::strtoul(res.c_str(), &rest, 10);
            const unsigned long parsed_h =
                (sep == std::string::npos) ? 0 : std::strtoul(res.c_str() + sep + 1, &rest, 10);
            if (parsed_w == 0 || parsed_h == 0)
            {
                std::fprintf(stderr, "[render-bench] bad resolution '%s' (want WxH)\n", argv[4]);
                return 2;
            }
            width = static_cast<std::uint32_t>(parsed_w);
            height = static_cast<std::uint32_t>(parsed_h);
        }
        if (frames == 0)
        {
            std::fprintf(stderr, "[render-bench] frames must be > 0\n");
            return 2;
        }

        int exit_code = 0;
        std::unique_ptr<IDevice> device = acquire_device(*rhi, exit_code);
        if (device == nullptr)
        {
            finish(exit_code);
            return exit_code;
        }
        lit::LitBenchResult result;
        if (!lit::bench_lit_frames(*device, width, height, warmup, frames, result))
        {
            std::fprintf(stderr, "[render-bench] FAIL: bench loop did not complete\n");
            finish(1);
            return 1;
        }
        std::printf("%s\n", lit::bench_result_json(result).c_str());
        finish(0);
        return 0;
    }

    std::fprintf(stderr,
                 "usage: %s [probe|render|sprite|lit|viewport|golden <scene> <out.ppm>|bench "
                 "[frames] [warmup] [WxH]]\n",
                 argv[0]);
    return 2;
}
