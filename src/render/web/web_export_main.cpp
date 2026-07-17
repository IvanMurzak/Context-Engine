// The T1 WEB EXPORT harness (M8 a11, R-BUILD-001 / L-56 / R-ASSET-003 / R-ASSET-005): the browser
// side of the `context build --target web` export bundle. It proves the packed web build (1) BOOTS on
// the browser's WebGPU (the same emdawnwebgpu backend the render-web harness uses) and (2) STREAMS its
// v1 chunked pack over HTTP RANGE requests into the a02 RuntimeContentLoader, holding the resident
// working set within a configured memory budget (WebPackStreamer). It is built by the SAME `render-web`
// CI job (emcc is CI-only) and driven by tools/web_golden_run.py in headless Chromium + SwiftShader —
// it fetches the served content/game.pack, streams it, POSTs the stream verdict, renders the triangle3d
// golden through the browser WebGPU backend (the boot proof), and POSTs the frame for the SSIM gate.
//
// Kernel-free by design: the streamer depends only on the a02 content loader (frozen pack format +
// canonical serializer, L-24) and the T1 render backend, so the export harness needs no
// editor/derivation surface under emscripten — mirroring web_main.cpp. Emscripten -sEXIT_RUNTIME=1
// makes main's return the process exit code: 0 = PASS, 77 = SKIP (no browser WebGPU / no adapter),
// 1 = FAIL. Opened manually (no collector) the POSTs are simply skipped and PASS/FAIL prints to console.

#include "context/render/golden.h"
#include "context/render/wgpu/wgpu_rhi.h"
#include "context/runtime/content/web_pack_stream.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/em_js.h>
#endif

using namespace context::render;
namespace content = context::runtime::content;

#ifdef __EMSCRIPTEN__
// The browser-side fetch() logic lives in EM_ASYNC_JS *named JS functions*, NOT inline EM_ASM blocks.
// EM_ASM stringifies only its FIRST macro argument, so any top-level comma inside the JS block (a
// multi-variable `const a = $1, b = $2;` declaration, or an object literal spanning a comma) is taken
// as a macro-argument separator: the tail of the JS then leaks out as trailing EM_ASM args that clang
// compiles as C++ and rejects under -Werror (object-literal `:` -> GNU field-designator, `'Range'` ->
// multichar character constant, `$2`/`$3` -> undeclared identifiers). EM_JS/EM_ASYNC_JS instead capture
// the whole body via variadic `#__VA_ARGS__` stringification (comma-safe) and pass real named
// parameters (no `$N`, so no -Wdollar-in-identifier-extension either) — the JS is never parsed as C++.
// -sASYNCIFY (CMakeLists.txt) lets each function `await` its fetch and block the calling C++ until the
// promise settles, replacing the old globalThis flag + emscripten_sleep polling loops.

// POST `size` bytes from the wasm heap at `data` to `path` on the page's own origin (the channel back
// to tools/web_golden_run.py). Returns 1 when a collector accepted the body (r.ok), else 0 (no
// collector listening on a manual run, or a network error). The body is COPIED out of the heap before
// the await — a typed-array view over the heap must not outlive an Asyncify yield.
EM_ASYNC_JS(int, ctx_export_post, (const char* path, const char* data, int size), {
    const url = UTF8ToString(path);
    const body = new Uint8Array(HEAPU8.subarray(data, data + size));
    try {
        const r = await fetch(url, { method: 'POST', body: body });
        return r.ok ? 1 : 0;
    } catch (e) {
        return 0;
    }
});

// HEAD `url` and return its Content-Length (>= 0 bytes), or -1 on a non-ok response / network error.
// A HEAD request yields Content-Length without transferring the body.
EM_ASYNC_JS(double, ctx_export_content_length, (const char* url), {
    const target = UTF8ToString(url);
    try {
        const r = await fetch(target, { method: 'HEAD' });
        if (!r.ok) return -1;
        return parseInt(r.headers.get('Content-Length') || '0', 10) || 0;
    } catch (e) {
        return -1;
    }
});

// Range-fetch [offset, offset + length) of `url` and copy the response bytes into the wasm heap at
// `dst`. Returns 1 on success, 0 on a wrong-length body or a non-206/200 error response. The heap is
// sized up front with NO -sALLOW_MEMORY_GROWTH (CMakeLists.txt), so `dst` (a pointer into a caller's
// pre-sized buffer) stays valid across the await.
EM_ASYNC_JS(int, ctx_export_fetch_range,
            (const char* url, double offset, double length, char* dst), {
    const target = UTF8ToString(url);
    const end = offset + length - 1;
    try {
        const r = await fetch(target, { headers: { 'Range': 'bytes=' + offset + '-' + end } });
        if (!r.ok && r.status !== 206 && r.status !== 200) return 0;
        const bytes = new Uint8Array(await r.arrayBuffer());
        if (bytes.length !== length) return 0;
        HEAPU8.set(bytes, dst);
        return 1;
    } catch (e) {
        return 0;
    }
});
#endif // __EMSCRIPTEN__

namespace
{

// The served pack URL (page-relative) + the streaming config. The pack ships under content/ in the
// export bundle; the CI driver serves it beside the harness page. 64 KiB range chunks + a 256 KiB
// resident memory budget exercise a genuine chunked transfer + a bounded working set on the small
// sample; a shipped game tunes these to the browser envelope (docs/export-adapters.md § Web).
constexpr const char* kPackUrl = "content/game.pack";
constexpr std::uint64_t kChunkBytes = 65536;
constexpr std::uint64_t kMemoryBudget = 262144;

// POST `size` bytes to `path` on the page's own origin — the channel back to tools/web_golden_run.py.
// Returns true when a collector accepted the body; false when none is listening (manual run) or on
// error. Delegates to the ctx_export_post EM_ASYNC_JS above (native builds are a no-op).
bool post_bytes(const char* path, const void* data, std::size_t size)
{
#ifdef __EMSCRIPTEN__
    return ctx_export_post(path, static_cast<const char*>(data), static_cast<int>(size)) == 1;
#else
    (void)path;
    (void)data;
    (void)size;
    return false;
#endif
}

void post_done(int exit_code)
{
    const std::string done = "/done?exit=" + std::to_string(exit_code);
    post_bytes(done.c_str(), "", 0);
}

// The a11 HTTP-range ChunkFetcher: pulls byte ranges of the served pack over `fetch()` with a Range
// header (the production browser transport for R-ASSET-003 streaming). Both methods delegate to the
// EM_ASYNC_JS functions above, which await the fetch (Asyncify) and copy the response into the wasm
// heap. Non-emscripten builds are a no-op (this TU only ships under emcc).
class HttpRangeFetcher final : public content::ChunkFetcher
{
public:
    explicit HttpRangeFetcher(std::string url) : url_(std::move(url)) {}

    [[nodiscard]] std::uint64_t total_size() const override
    {
#ifdef __EMSCRIPTEN__
        const double len = ctx_export_content_length(url_.c_str());
        return len > 0 ? static_cast<std::uint64_t>(len) : 0;
#else
        return 0;
#endif
    }

    [[nodiscard]] bool fetch_range(std::uint64_t offset, std::uint64_t length,
                                   std::string& out) const override
    {
#ifdef __EMSCRIPTEN__
        out.resize(static_cast<std::size_t>(length));
        return ctx_export_fetch_range(url_.c_str(), static_cast<double>(offset),
                                      static_cast<double>(length), out.data()) == 1;
#else
        (void)offset;
        (void)length;
        (void)out;
        return false;
#endif
    }

private:
    std::string url_;
};

} // namespace

int main()
{
    std::unique_ptr<IRhi> rhi = create_wgpu_rhi();
    if (rhi == nullptr)
    {
        std::printf("[web-export] SKIP: no WebGPU instance (browser WebGPU unavailable)\n");
        post_done(77);
        return 77;
    }
    std::printf("[web-export] backend=%s tier=T1\n", rhi->backend_name());
    const AdapterProbe probe = rhi->probe();
    if (!probe.has_adapter)
    {
        std::printf("[web-export] SKIP: no WebGPU adapter (navigator.gpu unavailable)\n");
        post_done(77);
        return 77;
    }
    std::unique_ptr<IDevice> device = rhi->create_device();
    if (device == nullptr)
    {
        std::fprintf(stderr, "[web-export] FAIL: device creation failed despite an adapter\n");
        post_done(1);
        return 1;
    }

    // --- a11 core: stream the served pack over HTTP range within the memory budget --------------------
    bool stream_ok = false;
    {
        HttpRangeFetcher fetcher(kPackUrl);
        const content::WebStreamResult res =
            content::stream_pack_chunked(fetcher, kChunkBytes, kMemoryBudget);
        if (!res.ok)
        {
            std::fprintf(stderr, "[web-export] FAIL: pack stream failed: %s\n", res.error.c_str());
        }
        else
        {
            stream_ok = res.peak_resident_bytes <= kMemoryBudget && res.resident_unit_count >= 1;
            std::printf("[web-export] streamed pack: units=%zu resident=%zu chunks=%zu peak=%llu "
                        "budget=%llu ok=%d\n",
                        res.unit_count, res.resident_unit_count, res.chunk_count,
                        static_cast<unsigned long long>(res.peak_resident_bytes),
                        static_cast<unsigned long long>(kMemoryBudget), stream_ok ? 1 : 0);
            // Report the stream verdict to the collector as a tiny JSON body (a manual run skips it).
            const std::string signal = "{\"streamOk\":" + std::string(stream_ok ? "true" : "false") +
                                       ",\"units\":" + std::to_string(res.unit_count) +
                                       ",\"resident\":" + std::to_string(res.resident_unit_count) +
                                       ",\"peak\":" + std::to_string(res.peak_resident_bytes) + "}";
            post_bytes("/webexport", signal.data(), signal.size());
        }
    }

    // --- boot proof: render the triangle3d golden through the browser WebGPU backend + POST it --------
    bool render_ok = false;
    {
        golden::GoldenImage image;
        if (!golden::render_golden_scene(*device, "triangle3d", image))
        {
            std::fprintf(stderr, "[web-export] FAIL: golden scene 'triangle3d' did not render\n");
        }
        else
        {
            render_ok = true;
            const std::string path = "/golden/triangle3d?w=" + std::to_string(image.width) +
                                     "&h=" + std::to_string(image.height);
            if (post_bytes(path.c_str(), image.rgba.data(), image.rgba.size()))
                std::printf("[web-export] golden scene=triangle3d (%ux%u) posted\n", image.width,
                            image.height);
            else
                std::printf("[web-export] golden scene=triangle3d rendered (no collector — manual)\n");
        }
    }

    int exit_code = (stream_ok && render_ok) ? 0 : 1;
    if (exit_code == 0)
        std::printf("[web-export] PASS (pack streamed within budget + browser WebGPU boot)\n");
    else
        std::fprintf(stderr, "[web-export] FAIL: stream=%d render=%d\n", stream_ok, render_ok);
    post_done(exit_code);
    return exit_code;
}
