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
// Built to wasm+JS+HTML by the `render-web` CI leg and RUN in headless Chromium by the same job's
// golden step (M4 T7, issue #141): tools/web_golden_run.py serves this page, and the harness POSTs
// each golden-corpus frame back to it for the SSIM visual-equivalence gate vs goldens/ — the M4
// "one browser blocking" run gate. Opened manually in a WebGPU browser the POSTs simply find no
// collector and are skipped (the proofs still print PASS/FAIL to the page + console). Emscripten
// -sEXIT_RUNTIME=1 makes main's return the process exit code: 0 = PASS, 77 = SKIP (no browser
// WebGPU / no adapter), 1 = FAIL.
//
// Kernel-free by design: the triangle + sprite proofs and the golden.h corpus pair drive only the
// RHI abstraction + the pure-CPU 2D math (ortho/atlas/batch), so the web build needs no
// kernel/extract/filesystem surface under emscripten. The lit/PBR (3D lighting+shadow) web proof,
// which pulls the kernel + extract, is a deferred follow-up — see src/render/web/README.md.

#include "context/render/golden.h"
#include "context/render/offscreen_scene.h"
#include "context/render/sprite/sprite_offscreen.h"
#include "context/render/wgpu/wgpu_rhi.h"

#include <cstdio>
#include <memory>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace context::render;

namespace
{

// POST `size` bytes to `path` on the page's own origin — the channel back to the golden collector
// (tools/web_golden_run.py). Returns true when a collector accepted the body; false when none is
// listening (the manual in-browser run) or the request errored. Console-log-free by design: pixel
// payloads travel as request bodies, never as console lines Chrome could truncate.
bool post_bytes(const char* path, const void* data, std::size_t size)
{
#ifdef __EMSCRIPTEN__
    // The fetch is async; ASYNCIFY's emscripten_sleep yields to the event loop until it settles.
    // The body is COPIED out of the wasm heap before fetch takes it (typed-array views over the
    // heap must not outlive a yield).
    EM_ASM(
        {
            globalThis.__ctxGoldenPost = 0;
            const path = UTF8ToString($0);
            const body = new Uint8Array(HEAPU8.subarray($1, $1 + $2));
            fetch(path, {method : 'POST', body : body})
                .then(function(r) { globalThis.__ctxGoldenPost = r.ok ? 1 : 2; })
                .catch(function() { globalThis.__ctxGoldenPost = 2; });
        },
        path, data, size);
    for (int waited_ms = 0; waited_ms < 20000; waited_ms += 50)
    {
        if (emscripten_run_script_int("globalThis.__ctxGoldenPost|0") != 0)
        {
            break;
        }
        emscripten_sleep(50);
    }
    return emscripten_run_script_int("globalThis.__ctxGoldenPost|0") == 1;
#else
    (void)path;
    (void)data;
    (void)size;
    return false;
#endif
}

// Tell the golden collector the harness is finished with `exit_code` (a no-op when nothing is
// listening — a manual in-browser run). EVERY harness exit path routes through here, including the
// early no-WebGPU / no-adapter / device-fail skips, so tools/web_golden_run.py always observes a
// /done and never blocks on its wait for the full --timeout on a degraded runner.
void post_done(int exit_code)
{
    const std::string done = "/done?exit=" + std::to_string(exit_code);
    post_bytes(done.c_str(), "", 0);
}

} // namespace

int main()
{
    std::unique_ptr<IRhi> rhi = create_wgpu_rhi();
    if (rhi == nullptr)
    {
        // On the web this is the "browser has no WebGPU" case — a clean skip, not a failure.
        std::printf("[render-web] SKIP: no WebGPU instance (browser WebGPU unavailable)\n");
        post_done(77);
        return 77;
    }
    std::printf("[render-web] backend=%s tier=T1\n", rhi->backend_name());

    // R-HEAD-002 seam: report adapter presence WITHOUT hard-failing when absent.
    const AdapterProbe probe = rhi->probe();
    if (!probe.has_adapter)
    {
        std::printf("[render-web] SKIP: no WebGPU adapter (navigator.gpu unavailable)\n");
        post_done(77);
        return 77;
    }

    std::unique_ptr<IDevice> device = rhi->create_device();
    if (device == nullptr)
    {
        std::fprintf(stderr, "[render-web] FAIL: device creation failed despite an adapter\n");
        post_done(1);
        return 1;
    }

    // 3D render pipeline (clip-space triangle) + 2D ortho sprite scene — the same T1 scenes native
    // renders, through the same rhi.h + WGSL.
    const bool triangle_ok = render_offscreen_triangle(*device) == OffscreenResult::Pass;
    const bool sprite_ok = sprite::render_offscreen_sprite(*device);

    // M4 T7 golden-scene corpus (issue #141): render each kernel-free corpus scene through THIS
    // browser backend and POST the raw RGBA frame to the collector when one is serving. A missing
    // collector (manual run) is fine; a scene that fails to RENDER is a real failure.
    bool goldens_ok = true;
    for (const std::string& scene : golden::kernel_free_scene_ids())
    {
        golden::GoldenImage image;
        if (!golden::render_golden_scene(*device, scene, image))
        {
            std::fprintf(stderr, "[render-web] FAIL: golden scene '%s' did not render\n",
                         scene.c_str());
            goldens_ok = false;
            continue;
        }
        const std::string path = "/golden/" + scene + "?w=" + std::to_string(image.width) +
                                 "&h=" + std::to_string(image.height);
        if (post_bytes(path.c_str(), image.rgba.data(), image.rgba.size()))
        {
            std::printf("[render-web] golden scene=%s (%ux%u) posted to collector\n",
                        scene.c_str(), image.width, image.height);
        }
        else
        {
            std::printf("[render-web] golden scene=%s rendered (no collector — manual run)\n",
                        scene.c_str());
        }
    }

    int exit_code = 1;
    if (triangle_ok && sprite_ok && goldens_ok)
    {
        std::printf("[render-web] PASS (triangle + sprite; browser WebGPU T1 parity with native)\n");
        exit_code = 0;
    }
    else
    {
        std::fprintf(stderr, "[render-web] FAIL: triangle=%d sprite=%d goldens=%d\n", triangle_ok,
                     sprite_ok, goldens_ok);
    }

    // Tell the collector the harness is complete (ignored when none is listening).
    post_done(exit_code);
    return exit_code;
}
