// The T1 WEB RHI backend offscreen parity harness (M4 T6, issue #137; R-REND-002, L-56, R-HEAD-002).
//
// Runs the SAME offscreen proofs the native backend runs — the 3D-clip-space render pipeline
// (offscreen_scene.h::render_offscreen_triangle) and the 2D ortho sprite path
// (sprite_offscreen.h::render_offscreen_sprite) — through the emdawnwebgpu WEB RHI backend
// (create_wgpu_rhi(), the same rhi.h implementation the native offscreen exe drives). Because it
// reuses the identical proof code + WGSL through the identical rhi.h object model, a pass here is
// desktop/web parity WITHIN the T1 feature set by construction (same semantics — bit-identical
// frames across backends are NOT required; float->unorm rounding legally differs per backend, see
// spikes/webgpu/FINDINGS.md).
//
// Built to wasm+JS+HTML by the `render-web` CI leg (the emscripten compile is the R-QA-013 gate for
// the web backend, mirroring the spike-webgpu-web precedent). The in-browser RUN — served + opened
// in a WebGPU browser — is a local/manual step today; the automated headless-browser golden-scene
// SSIM gate is the M4 T7 follow-up (ROADMAP §1). Emscripten -sEXIT_RUNTIME=1 makes main's return the
// process exit code: 0 = PASS, 77 = SKIP (no browser WebGPU / no adapter), 1 = FAIL.
//
// Kernel-free by design: the triangle + sprite proofs drive only the RHI abstraction + the pure-CPU
// 2D math (ortho/atlas/batch), so the web build needs no kernel/extract/filesystem surface under
// emscripten. The lit/PBR (3D lighting+shadow) web proof, which pulls the kernel + extract, lands
// with the T7 golden gate — see src/render/web/README.md.

#include "context/render/offscreen_scene.h"
#include "context/render/sprite/sprite_offscreen.h"
#include "context/render/wgpu/wgpu_rhi.h"

#include <cstdio>
#include <memory>

using namespace context::render;

int main()
{
    std::unique_ptr<IRhi> rhi = create_wgpu_rhi();
    if (rhi == nullptr)
    {
        // On the web this is the "browser has no WebGPU" case — a clean skip, not a failure.
        std::printf("[render-web] SKIP: no WebGPU instance (browser WebGPU unavailable)\n");
        return 77;
    }
    std::printf("[render-web] backend=%s tier=T1\n", rhi->backend_name());

    // R-HEAD-002 seam: report adapter presence WITHOUT hard-failing when absent.
    const AdapterProbe probe = rhi->probe();
    if (!probe.has_adapter)
    {
        std::printf("[render-web] SKIP: no WebGPU adapter (navigator.gpu unavailable)\n");
        return 77;
    }

    std::unique_ptr<IDevice> device = rhi->create_device();
    if (device == nullptr)
    {
        std::fprintf(stderr, "[render-web] FAIL: device creation failed despite an adapter\n");
        return 1;
    }

    // 3D render pipeline (clip-space triangle) + 2D ortho sprite scene — the same T1 scenes native
    // renders, through the same rhi.h + WGSL.
    const bool triangle_ok = render_offscreen_triangle(*device) == OffscreenResult::Pass;
    const bool sprite_ok = sprite::render_offscreen_sprite(*device);

    if (triangle_ok && sprite_ok)
    {
        std::printf("[render-web] PASS (triangle + sprite; browser WebGPU T1 parity with native)\n");
        return 0;
    }
    std::fprintf(stderr, "[render-web] FAIL: triangle=%d sprite=%d\n", triangle_ok, sprite_ok);
    return 1;
}
