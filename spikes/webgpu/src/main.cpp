// spikes/webgpu — WebGPU-baseline rendering spike (design locks L-11/L-56; R-REND-001/002,
// R-HEAD-002). THROWAWAY proof code, never production code.
//
// One triangle through `webgpu.h` semantics, proven three ways:
//   * `probe`  — R-HEAD-002 seam demo: create an instance, enumerate adapters, report them
//                (or cleanly report absence) WITHOUT ever creating a GPU device. Exit 0 always.
//   * `render` — (default) offscreen render + pixel readback self-check: draw the triangle
//                into a 256x256 RGBA8 texture, copy it to a mappable buffer, read the pixels
//                back, assert background/interior colors + analytic coverage, print PASS and
//                save an FNV-1a hash of the image. No window, no display, no swapchain —
//                runs headless on CI software adapters (lavapipe / WARP). If no adapter
//                exists, prints SKIP and exits 77 (ctest SKIP_RETURN_CODE) — absence of a
//                GPU is a clean skip, never a failure.
//   * `window` — (native Win32 only) the same triangle presented to a real window for ~2s,
//                the local "it really draws" sanity check. Not run on CI.
//
// The SAME source compiles for native (wgpu-native prebuilt, see CMakeLists.txt) and for the
// web (Emscripten + the `emdawnwebgpu` port binding the BROWSER's WebGPU — the locked web
// path per ARCHITECTURE.md §4; explicitly NOT a Dawn cross-compile). Divergences between the
// two `webgpu.h` snapshots are called out inline; see FINDINGS.md for the full list.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS // std::fopen for the hash artifact; fine for throwaway spike code
#endif

#include <webgpu/webgpu.h>

#if defined(CTX_SPIKE_HAS_WGPU_EXTRAS)
#include <webgpu/wgpu.h> // wgpu-native extras: wgpuInstanceEnumerateAdapters (not in emdawnwebgpu)
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#else
#include <chrono>
#include <thread>
#endif

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------- helpers

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;
constexpr uint32_t kBytesPerPixel = 4;
// bytesPerRow must be a multiple of 256 (COPY_BYTES_PER_ROW_ALIGNMENT); 256 px * 4 B = 1024 ✓.
constexpr uint32_t kBytesPerRow = kWidth * kBytesPerPixel;

WGPUStringView sv(const char* s) { return {s, std::strlen(s)}; }

std::string svToString(WGPUStringView view) {
    if (view.data == nullptr) return {};
    if (view.length == WGPU_STRLEN) return std::string(view.data);
    return std::string(view.data, view.length);
}

const char* backendName(WGPUBackendType b) {
    switch (b) {
        case WGPUBackendType_Null: return "Null";
        case WGPUBackendType_WebGPU: return "BrowserWebGPU";
        case WGPUBackendType_D3D11: return "D3D11";
        case WGPUBackendType_D3D12: return "D3D12";
        case WGPUBackendType_Metal: return "Metal";
        case WGPUBackendType_Vulkan: return "Vulkan";
        case WGPUBackendType_OpenGL: return "OpenGL";
        case WGPUBackendType_OpenGLES: return "OpenGLES";
        default: return "Unknown";
    }
}

const char* adapterTypeName(WGPUAdapterType t) {
    switch (t) {
        case WGPUAdapterType_DiscreteGPU: return "DiscreteGPU";
        case WGPUAdapterType_IntegratedGPU: return "IntegratedGPU";
        case WGPUAdapterType_CPU: return "CPU (software)";
        case WGPUAdapterType_Unknown: return "Unknown";
        default: return "Unknown";
    }
}

// One poll step: deliver completed async events, then yield briefly.
// Native: wgpuInstanceProcessEvents + a 1 ms sleep. Web (Asyncify): emscripten_sleep yields to
// the browser event loop so the underlying JS promises can resolve.
void pump(WGPUInstance instance) {
    wgpuInstanceProcessEvents(instance);
#if defined(__EMSCRIPTEN__)
    emscripten_sleep(1);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

void onUncapturedError(WGPUDevice const* /*device*/, WGPUErrorType type, WGPUStringView message,
                       void* /*userdata1*/, void* /*userdata2*/) {
    std::fprintf(stderr, "[webgpu-spike] UNCAPTURED ERROR (type %d): %s\n",
                 static_cast<int>(type), svToString(message).c_str());
}

void onDeviceLost(WGPUDevice const* /*device*/, WGPUDeviceLostReason reason, WGPUStringView message,
                  void* /*userdata1*/, void* /*userdata2*/) {
    // Destroyed / CallbackCancelled are expected teardown-order noise (instance released last).
    if (reason != WGPUDeviceLostReason_Destroyed &&
        reason != WGPUDeviceLostReason_CallbackCancelled) {
        std::fprintf(stderr, "[webgpu-spike] DEVICE LOST (reason %d): %s\n",
                     static_cast<int>(reason), svToString(message).c_str());
    }
}

// ------------------------------------------------------------------------- async wrappers

struct AdapterResult {
    WGPUAdapter adapter = nullptr;
    bool done = false;
    std::string message;
};

void onAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
               void* userdata1, void* /*userdata2*/) {
    auto* out = static_cast<AdapterResult*>(userdata1);
    out->adapter = (status == WGPURequestAdapterStatus_Success) ? adapter : nullptr;
    out->message = svToString(message);
    out->done = true;
}

// Set by `render --fallback`: request the JS-spec "fallback adapter" (software rasterizer —
// WARP on D3D12, llvmpipe on Vulkan), to exercise the exact path CI's GPU-less runners take.
bool g_forceFallbackAdapter = false;

WGPUAdapter requestAdapter(WGPUInstance instance, WGPUSurface compatibleSurface,
                           std::string* failureMessage) {
    WGPURequestAdapterOptions opts{};
    opts.compatibleSurface = compatibleSurface;
    opts.forceFallbackAdapter = g_forceFallbackAdapter ? WGPU_TRUE : WGPU_FALSE;

    AdapterResult result;
    WGPURequestAdapterCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = onAdapter;
    cb.userdata1 = &result;
    wgpuInstanceRequestAdapter(instance, &opts, cb);
    while (!result.done) pump(instance);
    if (failureMessage != nullptr) *failureMessage = result.message;
    return result.adapter;
}

struct DeviceResult {
    WGPUDevice device = nullptr;
    bool done = false;
    std::string message;
};

void onDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
              void* userdata1, void* /*userdata2*/) {
    auto* out = static_cast<DeviceResult*>(userdata1);
    out->device = (status == WGPURequestDeviceStatus_Success) ? device : nullptr;
    out->message = svToString(message);
    out->done = true;
}

WGPUDevice requestDevice(WGPUInstance instance, WGPUAdapter adapter) {
    WGPUDeviceDescriptor desc{};
    desc.label = sv("webgpu-spike-device");
    desc.uncapturedErrorCallbackInfo.callback = onUncapturedError;
    desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    desc.deviceLostCallbackInfo.callback = onDeviceLost;

    DeviceResult result;
    WGPURequestDeviceCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = onDevice;
    cb.userdata1 = &result;
    wgpuAdapterRequestDevice(adapter, &desc, cb);
    while (!result.done) pump(instance);
    if (result.device == nullptr) {
        std::fprintf(stderr, "[webgpu-spike] device request failed: %s\n", result.message.c_str());
    }
    return result.device;
}

// ------------------------------------------------------------------------------ the triangle

// WGSL — the ONLY shader language on the web path (SPIR-V ingestion does not exist in the
// browser API). Backend translation: wgpu-native lowers this through Naga; Dawn-based stacks
// (incl. the browser's Chrome/Tint) lower through Tint. See FINDINGS.md §WGSL toolchain.
constexpr const char* kTriangleWGSL = R"(
@vertex
fn vs_main(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {
    var p = array<vec2f, 3>(
        vec2f( 0.0,  0.5),
        vec2f(-0.5, -0.5),
        vec2f( 0.5, -0.5));
    return vec4f(p[i], 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4f {
    return vec4f(1.0, 0.0, 0.0, 1.0);
}
)";

WGPURenderPipeline createTrianglePipeline(WGPUDevice device, WGPUTextureFormat targetFormat) {
    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = sv(kTriangleWGSL);

    WGPUShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgsl.chain;
    smDesc.label = sv("triangle-wgsl");
    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(device, &smDesc);

    WGPUColorTargetState target{};
    target.format = targetFormat;
    target.writeMask = WGPUColorWriteMask_All; // zero-init means "write nothing" — must be explicit

    WGPUFragmentState fragment{};
    fragment.module = shader;
    fragment.entryPoint = sv("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &target;

    WGPURenderPipelineDescriptor desc{};
    desc.label = sv("triangle-pipeline");
    desc.vertex.module = shader;
    desc.vertex.entryPoint = sv("vs_main");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.primitive.frontFace = WGPUFrontFace_CCW;
    desc.primitive.cullMode = WGPUCullMode_None;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFFu;
    desc.fragment = &fragment;

    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
    wgpuShaderModuleRelease(shader);
    return pipeline;
}

void encodeTrianglePass(WGPUCommandEncoder encoder, WGPUTextureView view,
                        WGPURenderPipeline pipeline) {
    WGPURenderPassColorAttachment color{};
    color.view = view;
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED; // zero-init (0) fails validation on 2D views
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = {0.1, 0.2, 0.3, 1.0};

    WGPURenderPassDescriptor pass{};
    pass.label = sv("triangle-pass");
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;

    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
    wgpuRenderPassEncoderSetPipeline(rp, pipeline);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);
}

// -------------------------------------------------------------------------------- readback

struct MapResult {
    bool done = false;
    bool ok = false;
    std::string message;
};

void onMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1,
              void* /*userdata2*/) {
    auto* out = static_cast<MapResult*>(userdata1);
    out->ok = (status == WGPUMapAsyncStatus_Success);
    out->message = svToString(message);
    out->done = true;
}

uint64_t fnv1a64(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool pixelNear(const uint8_t* px, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tolerance) {
    // NB: not named `near` — windef.h #defines `near`/`far` away on Win32.
    auto closeTo = [tolerance](int actual, int expected) {
        int d = actual - expected;
        return d >= -tolerance && d <= tolerance;
    };
    return closeTo(px[0], r) && closeTo(px[1], g) && closeTo(px[2], b) && closeTo(px[3], a);
}

// Offscreen render + pixel readback self-check. Returns a process exit code:
// 0 = PASS, 77 = SKIP (no adapter), 1 = FAIL.
int runRenderSelfCheck(WGPUInstance instance) {
    std::string failure;
    WGPUAdapter adapter = requestAdapter(instance, nullptr, &failure);
    if (adapter == nullptr) {
        std::printf("[webgpu-spike] SKIP: no WebGPU adapter available (%s)\n",
                    failure.empty() ? "no message" : failure.c_str());
        return 77;
    }

    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        std::printf("[webgpu-spike] adapter: %s | backend=%s | type=%s\n",
                    svToString(info.device).c_str(), backendName(info.backendType),
                    adapterTypeName(info.adapterType));
        wgpuAdapterInfoFreeMembers(info);
    }

    WGPUDevice device = requestDevice(instance, adapter);
    if (device == nullptr) {
        wgpuAdapterRelease(adapter);
        return 1;
    }
    WGPUQueue queue = wgpuDeviceGetQueue(device);

    // Offscreen color target — RenderAttachment | CopySrc; no surface, no window, no present.
    WGPUTextureDescriptor texDesc{};
    texDesc.label = sv("offscreen-target");
    texDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    texDesc.dimension = WGPUTextureDimension_2D;
    texDesc.size = {kWidth, kHeight, 1};
    texDesc.format = WGPUTextureFormat_RGBA8Unorm;
    texDesc.mipLevelCount = 1;
    texDesc.sampleCount = 1;
    WGPUTexture texture = wgpuDeviceCreateTexture(device, &texDesc);
    WGPUTextureView view = wgpuTextureCreateView(texture, nullptr);

    WGPUBufferDescriptor bufDesc{};
    bufDesc.label = sv("readback");
    bufDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    bufDesc.size = static_cast<uint64_t>(kBytesPerRow) * kHeight;
    WGPUBuffer readback = wgpuDeviceCreateBuffer(device, &bufDesc);

    WGPURenderPipeline pipeline = createTrianglePipeline(device, WGPUTextureFormat_RGBA8Unorm);

    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    encodeTrianglePass(encoder, view, pipeline);

    WGPUTexelCopyTextureInfo src{};
    src.texture = texture;
    WGPUTexelCopyBufferInfo dst{};
    dst.layout.bytesPerRow = kBytesPerRow;
    dst.layout.rowsPerImage = kHeight;
    dst.buffer = readback;
    WGPUExtent3D extent = {kWidth, kHeight, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);

    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands);

    MapResult map;
    WGPUBufferMapCallbackInfo mapCb{};
    mapCb.mode = WGPUCallbackMode_AllowProcessEvents;
    mapCb.callback = onMapped;
    mapCb.userdata1 = &map;
    wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, bufDesc.size, mapCb);
    while (!map.done) pump(instance);

    int exitCode = 1;
    if (!map.ok) {
        std::fprintf(stderr, "[webgpu-spike] FAIL: buffer map failed: %s\n", map.message.c_str());
    } else {
        const auto* pixels =
            static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(readback, 0, bufDesc.size));

        // (a) background pixel — the clear color 0.1/0.2/0.3 → 25.5/51/76.5 in unorm8;
        //     float→unorm rounding may legally differ by 1 between backends, tolerance covers it.
        const uint8_t* bg = pixels + (8u * kBytesPerRow) + (8u * kBytesPerPixel);
        bool bgOk = pixelNear(bg, 26, 51, 77, 255, 6);

        // (b) a pixel well inside the triangle — solid red.
        //     NDC (0,0.5)/(±0.5,-0.5) → screen (128,64)/(64,192)/(192,192); centroid ≈ (128,149).
        const uint8_t* tri = pixels + (150u * kBytesPerRow) + (128u * kBytesPerPixel);
        bool triOk = pixelNear(tri, 255, 0, 0, 255, 2);

        // (c) analytic coverage — triangle area is 1/8 of the 4-unit² NDC clip square:
        //     0.5 * 128px * 128px = 8192px of 65536 = 12.5%. Assert 11%..14% (edge rules ±).
        uint32_t redCount = 0;
        for (uint32_t y = 0; y < kHeight; ++y) {
            const uint8_t* row = pixels + (static_cast<size_t>(y) * kBytesPerRow);
            for (uint32_t x = 0; x < kWidth; ++x) {
                const uint8_t* px = row + (static_cast<size_t>(x) * kBytesPerPixel);
                if (px[0] > 200 && px[1] < 60 && px[2] < 60) ++redCount;
            }
        }
        double coverage = 100.0 * redCount / (double(kWidth) * double(kHeight));

        // Image hash — informational identity of the readback (NOT asserted cross-platform:
        // unorm rounding + edge fill rules may differ by backend; see FINDINGS.md).
        uint64_t hash = fnv1a64(pixels, static_cast<size_t>(bufDesc.size));

        std::printf("[webgpu-spike] background(8,8)   = %u,%u,%u,%u  -> %s\n", bg[0], bg[1], bg[2],
                    bg[3], bgOk ? "ok" : "MISMATCH");
        std::printf("[webgpu-spike] triangle(128,150) = %u,%u,%u,%u  -> %s\n", tri[0], tri[1],
                    tri[2], tri[3], triOk ? "ok" : "MISMATCH");
        std::printf("[webgpu-spike] red coverage      = %.2f%% (expected 12.50%% analytic, "
                    "accept 11..14)\n", coverage);
        std::printf("[webgpu-spike] HASH=0x%016llx\n", static_cast<unsigned long long>(hash));

        bool coverageOk = coverage > 11.0 && coverage < 14.0;
        if (bgOk && triOk && coverageOk) {
            std::FILE* f = std::fopen("webgpu-spike-hash.txt", "w");
            if (f != nullptr) {
                std::fprintf(f, "fnv1a64 0x%016llx rgba8 %ux%u\n",
                             static_cast<unsigned long long>(hash), kWidth, kHeight);
                std::fclose(f);
            }
            std::printf("[webgpu-spike] PASS\n");
            exitCode = 0;
        } else {
            std::fprintf(stderr, "[webgpu-spike] FAIL: readback did not match expectations\n");
        }
        wgpuBufferUnmap(readback);
    }

    wgpuRenderPipelineRelease(pipeline);
    wgpuBufferRelease(readback);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(texture);
    wgpuQueueRelease(queue);
    wgpuDeviceRelease(device);
    wgpuAdapterRelease(adapter);
    return exitCode;
}

// ------------------------------------------------------------------------------- probe mode

// R-HEAD-002 seam demo: instance + adapter enumeration ONLY — no wgpuAdapterRequestDevice call
// exists on this path, so no GPU device is ever created. Absence of adapters is a clean,
// successful report (exit 0), which is exactly the headless contract.
int runProbe(WGPUInstance instance) {
#if defined(CTX_SPIKE_HAS_WGPU_EXTRAS)
    size_t count = wgpuInstanceEnumerateAdapters(instance, nullptr, nullptr);
    std::printf("[webgpu-spike] probe: %zu adapter(s) enumerated (no GPU device created)\n", count);
    if (count == 0) {
        std::printf("[webgpu-spike] probe: no adapters — headless path confirmed, "
                    "reporting absence cleanly\n");
        return 0;
    }
    std::vector<WGPUAdapter> adapters(count);
    wgpuInstanceEnumerateAdapters(instance, nullptr, adapters.data());
    for (size_t i = 0; i < count; ++i) {
        WGPUAdapterInfo info{};
        if (wgpuAdapterGetInfo(adapters[i], &info) == WGPUStatus_Success) {
            std::printf("[webgpu-spike]   #%zu: %s | vendor=%s | backend=%s | type=%s\n", i,
                        svToString(info.device).c_str(), svToString(info.vendor).c_str(),
                        backendName(info.backendType), adapterTypeName(info.adapterType));
            wgpuAdapterInfoFreeMembers(info);
        }
        wgpuAdapterRelease(adapters[i]);
    }
#else
    // emdawnwebgpu has no enumerate-adapters extra; requestAdapter (still no device) instead.
    std::string failure;
    WGPUAdapter adapter = requestAdapter(instance, nullptr, &failure);
    if (adapter == nullptr) {
        std::printf("[webgpu-spike] probe: no adapter (%s) — absence reported cleanly, "
                    "no GPU device created\n", failure.empty() ? "no message" : failure.c_str());
        return 0;
    }
    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        std::printf("[webgpu-spike] probe: adapter %s | backend=%s | type=%s "
                    "(no GPU device created)\n", svToString(info.device).c_str(),
                    backendName(info.backendType), adapterTypeName(info.adapterType));
        wgpuAdapterInfoFreeMembers(info);
    }
    wgpuAdapterRelease(adapter);
#endif
    return 0;
}

// ------------------------------------------------------------------- windowed sanity (Win32)

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)

LRESULT CALLBACK spikeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int runWindowed(WGPUInstance instance, int frames) {
    const HINSTANCE hinstance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = spikeWndProc;
    wc.hInstance = hinstance;
    wc.lpszClassName = L"ContextWebGPUSpike";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW, UNICODE-agnostic
    RegisterClassW(&wc);
    // ASCII-only title: a non-ASCII wide literal renders as mojibake unless the TU is built /utf-8.
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Context Engine - WebGPU spike (wgpu-native)",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                                nullptr, nullptr, hinstance, nullptr);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "[webgpu-spike] FAIL: CreateWindowExW failed\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);

    WGPUSurfaceSourceWindowsHWND source{};
    source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
    source.hinstance = hinstance;
    source.hwnd = hwnd;
    WGPUSurfaceDescriptor surfDesc{};
    surfDesc.nextInChain = &source.chain;
    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surfDesc);

    std::string failure;
    WGPUAdapter adapter = requestAdapter(instance, surface, &failure);
    if (adapter == nullptr) {
        std::printf("[webgpu-spike] SKIP: no adapter compatible with the window surface (%s)\n",
                    failure.c_str());
        wgpuSurfaceRelease(surface);
        DestroyWindow(hwnd);
        return 77;
    }
    WGPUDevice device = requestDevice(instance, adapter);
    if (device == nullptr) return 1;
    WGPUQueue queue = wgpuDeviceGetQueue(device);

    WGPUSurfaceCapabilities caps{};
    wgpuSurfaceGetCapabilities(surface, adapter, &caps);
    WGPUTextureFormat format =
        (caps.formatCount > 0) ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
    wgpuSurfaceCapabilitiesFreeMembers(caps);

    RECT rect{};
    GetClientRect(hwnd, &rect);
    WGPUSurfaceConfiguration config{};
    config.device = device;
    config.format = format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = static_cast<uint32_t>(rect.right - rect.left);
    config.height = static_cast<uint32_t>(rect.bottom - rect.top);
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &config);

    WGPURenderPipeline pipeline = createTrianglePipeline(device, format);

    std::printf("[webgpu-spike] windowed: %ux%u, format %d — presenting ~%d frames\n",
                config.width, config.height, static_cast<int>(format), frames);
    bool alive = true;
    for (int frame = 0; frame < frames && alive; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) alive = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        WGPUSurfaceTexture frameTex{};
        wgpuSurfaceGetCurrentTexture(surface, &frameTex);
        if (frameTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            frameTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            std::fprintf(stderr, "[webgpu-spike] FAIL: GetCurrentTexture status %d\n",
                         static_cast<int>(frameTex.status));
            alive = false;
            break;
        }
        WGPUTextureView view = wgpuTextureCreateView(frameTex.texture, nullptr);
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        encodeTrianglePass(encoder, view, pipeline);
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuCommandEncoderRelease(encoder);
        wgpuQueueSubmit(queue, 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuSurfacePresent(surface);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(frameTex.texture);
    }
    std::printf("[webgpu-spike] windowed sanity done\n");

    wgpuRenderPipelineRelease(pipeline);
    wgpuSurfaceUnconfigure(surface);
    wgpuQueueRelease(queue);
    wgpuDeviceRelease(device);
    wgpuAdapterRelease(adapter);
    wgpuSurfaceRelease(surface);
    DestroyWindow(hwnd);
    return 0;
}

#endif // _WIN32 windowed sanity

} // namespace

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "render";

    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (instance == nullptr) {
        // On the web this is the "browser has no WebGPU" case — a clean skip, not a failure.
        std::printf("[webgpu-spike] SKIP: wgpuCreateInstance returned null (WebGPU unavailable)\n");
        return (mode == "probe") ? 0 : 77;
    }

    int exitCode;
    if (mode == "probe") {
        exitCode = runProbe(instance);
    } else if (mode == "render") {
        g_forceFallbackAdapter = (argc > 2 && std::string(argv[2]) == "--fallback");
        exitCode = runRenderSelfCheck(instance);
    } else if (mode == "window") {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
        const int frames = (argc > 2) ? std::atoi(argv[2]) : 120; // ~2 s at 60 Hz Fifo
        exitCode = runWindowed(instance, frames > 0 ? frames : 120);
#else
        std::fprintf(stderr, "[webgpu-spike] window mode is Win32-only in this spike; "
                             "use 'render' (offscreen) instead\n");
        exitCode = 2;
#endif
    } else {
        std::fprintf(stderr, "usage: %s [probe|render|window]\n", argv[0]);
        exitCode = 2;
    }

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // Windows teardown-race bypass (Context-Engine#210). wgpu-native spins up D3D12/Vulkan driver
    // threads (adapter enumeration in `probe`, device work in `render`); on the Session-0
    // LocalSystem CI runners its global/driver-thread teardown intermittently __fastfail's with
    // 0xc0000409 (STATUS_STACK_BUFFER_OVERRUN) at process exit — NON-deterministically and
    // REGARDLESS of adapter or mode (see CMakeLists.txt). The result is already computed and the
    // spike writes no files past this point (the render hash artifact is written+closed inside
    // runRenderSelfCheck), so flush the reports and skip wgpuInstanceRelease + the Rust-runtime
    // teardown entirely — the OS reclaims every resource on exit. This makes the Windows probe
    // DETERMINISTIC BY CONSTRUCTION (the racy teardown code never runs), not merely rarer; a
    // single green run could never prove a probabilistic fix. Non-Windows paths (Linux lavapipe /
    // macOS / web-browser render self-checks) keep the full, validated teardown below untouched.
    std::fflush(nullptr); // quick_exit does not flush stdio buffers
    std::quick_exit(exitCode);
#else
    wgpuInstanceRelease(instance);
    return exitCode;
#endif
}
