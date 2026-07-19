// The T1 RHI backend over webgpu.h (R-REND-001/002, L-56) — see wgpu/wgpu_rhi.h.
//
// Implements the rhi.h object model over webgpu.h, faithfully reusing the wgpu calls proven by the
// throwaway spikes/webgpu spike (adapter/device request + pump loop, offscreen texture + readback
// buffer, WGSL pipeline, render pass, texture->buffer copy, async map). This TU is the ONLY place
// webgpu.h is included. It compiles two ways from ONE source (the spike's proven "zero #ifdef in the
// API-usage code" property):
//   * NATIVE — the wgpu-native PREBUILT, compiled under CONTEXT_BUILD_RENDER_WGPU (a CI-gated
//     dependency path, like V8, because the sole off-the-shelf Windows prebuilt is MSVC-ABI).
//   * WEB (M4 T6, issue #137) — the BROWSER's WebGPU via Emscripten + the emdawnwebgpu port
//     (webgpu.h -> navigator.gpu; the L-56-locked web path, NOT a Dawn cross-compile), built by the
//     standalone src/render/web/ target under emcc. The ONLY divergences vs native are the three the
//     spike documented and are gated on __EMSCRIPTEN__ here: the poll pump yields to the browser
//     event loop (Asyncify emscripten_sleep) instead of a thread sleep, the R-HEAD-002 probe has no
//     enumerate-adapters extra (emdawnwebgpu offers requestAdapter only — the CTX_RENDER_HAS_WGPU_
//     EXTRAS-off fallback below), and backend_name() reports the browser backend. It is otherwise
//     platform-neutral (no window/surface code — offscreen only).

#include "context/render/wgpu/wgpu_rhi.h"

// The macOS accelerated OSR path needs BOTH conditions: Apple (Metal + IOSurface exist) and
// wgpu-native's extras header, which is where the STOCK native Metal accessors added upstream in
// PR #557 are declared. Anywhere else the accelerated import fails closed with a reason.
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__) && defined(CTX_RENDER_HAS_WGPU_EXTRAS)
#define CTX_RENDER_HAS_METAL_INTEROP 1
#include "metal_interop.h" // IOSurface -> Metal blit (Objective-C++, metal_interop.mm)
#endif

#include <webgpu/webgpu.h>

#if defined(CTX_RENDER_HAS_WGPU_EXTRAS)
#include <webgpu/wgpu.h> // wgpuInstanceEnumerateAdapters — the R-HEAD-002 device-free probe.
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h> // emscripten_sleep — yield to the browser event loop (Asyncify).
#else
#include <chrono>
#include <thread>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace context::render
{
namespace
{

WGPUStringView sv(const char* s)
{
    return {s, std::strlen(s)};
}

std::string sv_to_string(WGPUStringView view)
{
    if (view.data == nullptr)
    {
        return {};
    }
    if (view.length == WGPU_STRLEN)
    {
        return std::string(view.data);
    }
    return std::string(view.data, view.length);
}

// One poll step: deliver completed async events, then yield briefly so the pending async request
// (adapter/device/map) can resolve. Native pumps events + sleeps a thread; on the web the browser
// owns the event loop, so we yield to it via Asyncify (emscripten_sleep) — WebGPU promises resolve
// only when control returns to JS. This is the sole pump divergence the spike documented.
void pump(WGPUInstance instance)
{
    wgpuInstanceProcessEvents(instance);
#if defined(__EMSCRIPTEN__)
    emscripten_sleep(1); // Asyncify: yield to the browser event loop (needs -sASYNCIFY).
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

void on_uncaptured_error(WGPUDevice const* /*device*/, WGPUErrorType type, WGPUStringView message,
                         void* /*u1*/, void* /*u2*/)
{
    std::fprintf(stderr, "[render-wgpu] UNCAPTURED ERROR (type %d): %s\n", static_cast<int>(type),
                 sv_to_string(message).c_str());
}

void on_device_lost(WGPUDevice const* /*device*/, WGPUDeviceLostReason reason, WGPUStringView message,
                    void* /*u1*/, void* /*u2*/)
{
    if (reason != WGPUDeviceLostReason_Destroyed &&
        reason != WGPUDeviceLostReason_CallbackCancelled)
    {
        std::fprintf(stderr, "[render-wgpu] DEVICE LOST (reason %d): %s\n", static_cast<int>(reason),
                     sv_to_string(message).c_str());
    }
}

WGPUTextureFormat to_wgpu_format(TextureFormat f)
{
    switch (f)
    {
    case TextureFormat::BGRA8Unorm:
        return WGPUTextureFormat_BGRA8Unorm;
    case TextureFormat::Depth32Float:
        return WGPUTextureFormat_Depth32Float;
    case TextureFormat::RGBA8Unorm:
    default:
        return WGPUTextureFormat_RGBA8Unorm;
    }
}

WGPULoadOp to_wgpu_load(LoadOp op)
{
    return op == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
}

WGPUStoreOp to_wgpu_store(StoreOp op)
{
    return op == StoreOp::Discard ? WGPUStoreOp_Discard : WGPUStoreOp_Store;
}

WGPUCompareFunction to_wgpu_compare(CompareFunction f)
{
    switch (f)
    {
    case CompareFunction::Never:
        return WGPUCompareFunction_Never;
    case CompareFunction::Less:
        return WGPUCompareFunction_Less;
    case CompareFunction::LessEqual:
        return WGPUCompareFunction_LessEqual;
    case CompareFunction::Greater:
        return WGPUCompareFunction_Greater;
    case CompareFunction::GreaterEqual:
        return WGPUCompareFunction_GreaterEqual;
    case CompareFunction::Equal:
        return WGPUCompareFunction_Equal;
    case CompareFunction::NotEqual:
        return WGPUCompareFunction_NotEqual;
    case CompareFunction::Always:
        return WGPUCompareFunction_Always;
    case CompareFunction::Undefined:
    default:
        return WGPUCompareFunction_Undefined;
    }
}

WGPUFilterMode to_wgpu_filter(FilterMode f)
{
    return f == FilterMode::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
}

WGPUAddressMode to_wgpu_address(AddressMode m)
{
    return m == AddressMode::Repeat ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
}

WGPUBlendFactor to_wgpu_blend_factor(BlendFactor f)
{
    switch (f)
    {
    case BlendFactor::Zero:
        return WGPUBlendFactor_Zero;
    case BlendFactor::SrcAlpha:
        return WGPUBlendFactor_SrcAlpha;
    case BlendFactor::OneMinusSrcAlpha:
        return WGPUBlendFactor_OneMinusSrcAlpha;
    case BlendFactor::One:
    default:
        return WGPUBlendFactor_One;
    }
}

WGPUBlendComponent to_wgpu_blend_component(const BlendComponent& c)
{
    WGPUBlendComponent out{};
    out.operation = WGPUBlendOperation_Add; // Add is the only T1 operation; rhi.h stores no other
    out.srcFactor = to_wgpu_blend_factor(c.src);
    out.dstFactor = to_wgpu_blend_factor(c.dst);
    return out;
}

// The present-path conversions are used ONLY by WgpuSurface/WgpuSwapchain, which are compiled out on
// the web (no native window there — see create_surface). They must carry the SAME guard: an
// anonymous-namespace function with no callers is an unused static, which -Wall -Werror rejects, and
// that break would appear on the `render (web, emscripten)` CI leg alone.
#if !defined(__EMSCRIPTEN__)

WGPUPresentMode to_wgpu_present_mode(PresentMode mode)
{
    switch (mode)
    {
    case PresentMode::FifoRelaxed:
        return WGPUPresentMode_FifoRelaxed;
    case PresentMode::Immediate:
        return WGPUPresentMode_Immediate;
    case PresentMode::Mailbox:
        return WGPUPresentMode_Mailbox;
    case PresentMode::Fifo:
    default:
        return WGPUPresentMode_Fifo;
    }
}

// The reverse mappings translate what a surface REPORTS. Anything outside the T1 set is dropped
// rather than guessed at: a capability we cannot name is a capability we must not offer.
bool from_wgpu_format(WGPUTextureFormat format, TextureFormat& out)
{
    switch (format)
    {
    case WGPUTextureFormat_BGRA8Unorm:
        out = TextureFormat::BGRA8Unorm;
        return true;
    case WGPUTextureFormat_RGBA8Unorm:
        out = TextureFormat::RGBA8Unorm;
        return true;
    case WGPUTextureFormat_Depth32Float:
        out = TextureFormat::Depth32Float;
        return true;
    default:
        return false;
    }
}

bool from_wgpu_present_mode(WGPUPresentMode mode, PresentMode& out)
{
    switch (mode)
    {
    case WGPUPresentMode_Fifo:
        out = PresentMode::Fifo;
        return true;
    case WGPUPresentMode_FifoRelaxed:
        out = PresentMode::FifoRelaxed;
        return true;
    case WGPUPresentMode_Immediate:
        out = PresentMode::Immediate;
        return true;
    case WGPUPresentMode_Mailbox:
        out = PresentMode::Mailbox;
        return true;
    default:
        return false;
    }
}

#endif // !__EMSCRIPTEN__

// -------------------------------------------------------------------- async request wrappers

struct AdapterResult
{
    WGPUAdapter adapter = nullptr;
    bool done = false;
    std::string message;
};

void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                void* u1, void* /*u2*/)
{
    auto* out = static_cast<AdapterResult*>(u1);
    out->adapter = (status == WGPURequestAdapterStatus_Success) ? adapter : nullptr;
    out->message = sv_to_string(message);
    out->done = true;
}

// The ONE blocking adapter request. `compatible_surface` non-null asks for an adapter that can
// PRESENT to that surface; null asks for any adapter (the offscreen/headless probe). Both callers
// share the pump loop so a future change to it cannot land on only one of them.
WGPUAdapter request_adapter(WGPUInstance instance, std::string* failure,
                            WGPUSurface compatible_surface = nullptr)
{
    WGPURequestAdapterOptions opts{};
    opts.compatibleSurface = compatible_surface;
    AdapterResult result;
    WGPURequestAdapterCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = on_adapter;
    cb.userdata1 = &result;
    wgpuInstanceRequestAdapter(instance, &opts, cb);
    while (!result.done)
    {
        pump(instance);
    }
    if (failure != nullptr)
    {
        *failure = result.message;
    }
    return result.adapter;
}

struct DeviceResult
{
    WGPUDevice device = nullptr;
    bool done = false;
    std::string message;
};

void on_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* u1,
               void* /*u2*/)
{
    auto* out = static_cast<DeviceResult*>(u1);
    out->device = (status == WGPURequestDeviceStatus_Success) ? device : nullptr;
    out->message = sv_to_string(message);
    out->done = true;
}

WGPUDevice request_device(WGPUInstance instance, WGPUAdapter adapter)
{
    WGPUDeviceDescriptor desc{};
    desc.label = sv("context-render-device");
    desc.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
    desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    desc.deviceLostCallbackInfo.callback = on_device_lost;

    DeviceResult result;
    WGPURequestDeviceCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = on_device;
    cb.userdata1 = &result;
    wgpuAdapterRequestDevice(adapter, &desc, cb);
    while (!result.done)
    {
        pump(instance);
    }
    if (result.device == nullptr)
    {
        std::fprintf(stderr, "[render-wgpu] device request failed: %s\n", result.message.c_str());
    }
    return result.device;
}

struct MapResult
{
    bool done = false;
    bool ok = false;
    std::string message;
};

void on_mapped(WGPUMapAsyncStatus status, WGPUStringView message, void* u1, void* /*u2*/)
{
    auto* out = static_cast<MapResult*>(u1);
    out->ok = (status == WGPUMapAsyncStatus_Success);
    out->message = sv_to_string(message);
    out->done = true;
}

// ------------------------------------------------------------------------------- rhi objects

class WgpuTextureView final : public ITextureView
{
public:
    explicit WgpuTextureView(WGPUTextureView view) : view_(view) {}
    ~WgpuTextureView() override
    {
        if (view_ != nullptr)
        {
            wgpuTextureViewRelease(view_);
        }
    }
    [[nodiscard]] WGPUTextureView raw() const { return view_; }

private:
    WGPUTextureView view_;
};

class WgpuTexture final : public ITexture
{
public:
    explicit WgpuTexture(WGPUTexture texture) : texture_(texture) {}
    ~WgpuTexture() override
    {
        if (texture_ != nullptr)
        {
            wgpuTextureRelease(texture_);
        }
    }
    std::unique_ptr<ITextureView> create_view() override
    {
        return std::make_unique<WgpuTextureView>(wgpuTextureCreateView(texture_, nullptr));
    }
    [[nodiscard]] WGPUTexture raw() const { return texture_; }

private:
    WGPUTexture texture_;
};

class WgpuBuffer final : public IBuffer
{
public:
    WgpuBuffer(WGPUInstance instance, WGPUBuffer buffer, std::uint64_t size)
        : instance_(instance), buffer_(buffer), size_(size)
    {
    }
    ~WgpuBuffer() override
    {
        if (buffer_ != nullptr)
        {
            wgpuBufferRelease(buffer_);
        }
    }

    [[nodiscard]] std::uint64_t size() const override { return size_; }

    const void* map_read() override
    {
        MapResult map;
        WGPUBufferMapCallbackInfo cb{};
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_mapped;
        cb.userdata1 = &map;
        wgpuBufferMapAsync(buffer_, WGPUMapMode_Read, 0, size_, cb);
        while (!map.done)
        {
            pump(instance_);
        }
        if (!map.ok)
        {
            std::fprintf(stderr, "[render-wgpu] buffer map failed: %s\n", map.message.c_str());
            return nullptr;
        }
        return wgpuBufferGetConstMappedRange(buffer_, 0, size_);
    }

    void unmap() override { wgpuBufferUnmap(buffer_); }

    [[nodiscard]] WGPUBuffer raw() const { return buffer_; }

private:
    WGPUInstance instance_;
    WGPUBuffer buffer_;
    std::uint64_t size_;
};

class WgpuSampler final : public ISampler
{
public:
    explicit WgpuSampler(WGPUSampler sampler) : sampler_(sampler) {}
    ~WgpuSampler() override
    {
        if (sampler_ != nullptr)
        {
            wgpuSamplerRelease(sampler_);
        }
    }
    [[nodiscard]] WGPUSampler raw() const { return sampler_; }

private:
    WGPUSampler sampler_;
};

class WgpuBindGroupLayout final : public IBindGroupLayout
{
public:
    explicit WgpuBindGroupLayout(WGPUBindGroupLayout layout) : layout_(layout) {}
    ~WgpuBindGroupLayout() override
    {
        if (layout_ != nullptr)
        {
            wgpuBindGroupLayoutRelease(layout_);
        }
    }
    [[nodiscard]] WGPUBindGroupLayout raw() const { return layout_; }

private:
    WGPUBindGroupLayout layout_;
};

class WgpuBindGroup final : public IBindGroup
{
public:
    explicit WgpuBindGroup(WGPUBindGroup group) : group_(group) {}
    ~WgpuBindGroup() override
    {
        if (group_ != nullptr)
        {
            wgpuBindGroupRelease(group_);
        }
    }
    [[nodiscard]] WGPUBindGroup raw() const { return group_; }

private:
    WGPUBindGroup group_;
};

class WgpuRenderPipeline final : public IRenderPipeline
{
public:
    explicit WgpuRenderPipeline(WGPURenderPipeline pipeline) : pipeline_(pipeline) {}
    ~WgpuRenderPipeline() override
    {
        if (pipeline_ != nullptr)
        {
            wgpuRenderPipelineRelease(pipeline_);
        }
    }

    std::unique_ptr<IBindGroupLayout> bind_group_layout(std::uint32_t group) override
    {
        // Reflect the auto-generated layout from the pipeline's shader (rhi.h's only layout source
        // — the seam that stays correct under Tint's combined-sampler binding renumbering, T3d).
        return std::make_unique<WgpuBindGroupLayout>(
            wgpuRenderPipelineGetBindGroupLayout(pipeline_, group));
    }

    [[nodiscard]] WGPURenderPipeline raw() const { return pipeline_; }

private:
    WGPURenderPipeline pipeline_;
};

class WgpuRenderPassEncoder final : public IRenderPassEncoder
{
public:
    explicit WgpuRenderPassEncoder(WGPURenderPassEncoder pass) : pass_(pass) {}
    ~WgpuRenderPassEncoder() override
    {
        if (pass_ != nullptr)
        {
            wgpuRenderPassEncoderRelease(pass_);
        }
    }
    void set_pipeline(IRenderPipeline& pipeline) override
    {
        wgpuRenderPassEncoderSetPipeline(pass_, static_cast<WgpuRenderPipeline&>(pipeline).raw());
    }
    void set_bind_group(std::uint32_t index, IBindGroup& group) override
    {
        wgpuRenderPassEncoderSetBindGroup(pass_, index, static_cast<WgpuBindGroup&>(group).raw(), 0,
                                          nullptr);
    }
    void set_scissor_rect(Origin2D origin, Extent2D size) override
    {
        wgpuRenderPassEncoderSetScissorRect(pass_, origin.x, origin.y, size.width, size.height);
    }
    void draw(std::uint32_t vertex_count, std::uint32_t instance_count) override
    {
        wgpuRenderPassEncoderDraw(pass_, vertex_count, instance_count, 0, 0);
    }
    void end() override
    {
        wgpuRenderPassEncoderEnd(pass_);
        wgpuRenderPassEncoderRelease(pass_);
        pass_ = nullptr;
    }

private:
    WGPURenderPassEncoder pass_;
};

class WgpuCommandBuffer final : public ICommandBuffer
{
public:
    explicit WgpuCommandBuffer(WGPUCommandBuffer commands) : commands_(commands) {}
    ~WgpuCommandBuffer() override
    {
        if (commands_ != nullptr)
        {
            wgpuCommandBufferRelease(commands_);
        }
    }
    [[nodiscard]] WGPUCommandBuffer raw() const { return commands_; }

private:
    WGPUCommandBuffer commands_;
};

class WgpuCommandEncoder final : public ICommandEncoder
{
public:
    explicit WgpuCommandEncoder(WGPUCommandEncoder encoder) : encoder_(encoder) {}
    ~WgpuCommandEncoder() override
    {
        if (encoder_ != nullptr)
        {
            wgpuCommandEncoderRelease(encoder_);
        }
    }

    std::unique_ptr<IRenderPassEncoder> begin_render_pass(const RenderPassDesc& desc) override
    {
        std::vector<WGPURenderPassColorAttachment> colors;
        colors.reserve(desc.color.size());
        for (const ColorAttachment& a : desc.color)
        {
            WGPURenderPassColorAttachment color{};
            color.view = (a.view != nullptr) ? static_cast<WgpuTextureView*>(a.view)->raw()
                                             : nullptr;
            color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED; // zero (0) fails validation on 2D views
            color.loadOp = to_wgpu_load(a.load);
            color.storeOp = to_wgpu_store(a.store);
            color.clearValue = {a.clear.r, a.clear.g, a.clear.b, a.clear.a};
            colors.push_back(color);
        }
        WGPURenderPassDescriptor pass{};
        pass.label = sv("context-render-pass");
        pass.colorAttachmentCount = colors.size();
        pass.colorAttachments = colors.data();
        WGPURenderPassDepthStencilAttachment depth{};
        if (desc.depth.has_value() && desc.depth->view != nullptr)
        {
            depth.view = static_cast<WgpuTextureView*>(desc.depth->view)->raw();
            depth.depthLoadOp = to_wgpu_load(desc.depth->load);
            depth.depthStoreOp = to_wgpu_store(desc.depth->store);
            depth.depthClearValue = desc.depth->clear_depth;
            // No stencil aspect on Depth32Float: leave the stencil ops Undefined (zero-init).
            pass.depthStencilAttachment = &depth;
        }
        return std::make_unique<WgpuRenderPassEncoder>(
            wgpuCommandEncoderBeginRenderPass(encoder_, &pass));
    }

    void copy_texture_to_buffer(ITexture& src, IBuffer& dst, const TexelCopyBufferLayout& layout,
                                Extent2D extent) override
    {
        WGPUTexelCopyTextureInfo copy_src{};
        copy_src.texture = static_cast<WgpuTexture&>(src).raw();
        WGPUTexelCopyBufferInfo copy_dst{};
        copy_dst.layout.bytesPerRow = layout.bytes_per_row;
        copy_dst.layout.rowsPerImage = layout.rows_per_image;
        copy_dst.buffer = static_cast<WgpuBuffer&>(dst).raw();
        WGPUExtent3D size = {extent.width, extent.height, 1};
        wgpuCommandEncoderCopyTextureToBuffer(encoder_, &copy_src, &copy_dst, &size);
    }

    std::unique_ptr<ICommandBuffer> finish() override
    {
        return std::make_unique<WgpuCommandBuffer>(wgpuCommandEncoderFinish(encoder_, nullptr));
    }

private:
    WGPUCommandEncoder encoder_;
};

class WgpuQueue final : public IQueue
{
public:
    explicit WgpuQueue(WGPUQueue queue) : queue_(queue) {}
    ~WgpuQueue() override
    {
        if (queue_ != nullptr)
        {
            wgpuQueueRelease(queue_);
        }
    }
    void submit(ICommandBuffer& commands) override
    {
        WGPUCommandBuffer raw = static_cast<WgpuCommandBuffer&>(commands).raw();
        wgpuQueueSubmit(queue_, 1, &raw);
    }

    void write_buffer(IBuffer& buffer, std::uint64_t offset, const void* data,
                      std::size_t size) override
    {
        wgpuQueueWriteBuffer(queue_, static_cast<WgpuBuffer&>(buffer).raw(), offset, data, size);
    }

    void write_texture_region(ITexture& texture, Origin2D origin, const void* data,
                              std::size_t size, const TexelCopyBufferLayout& layout,
                              Extent2D extent) override
    {
        WGPUTexelCopyTextureInfo dst{};
        dst.texture = static_cast<WgpuTexture&>(texture).raw();
        // The destination sub-rect — what makes an OSR dirty rect land where the producer damaged
        // it instead of at the texture's top-left corner.
        dst.origin = {origin.x, origin.y, 0};
        WGPUTexelCopyBufferLayout src_layout{};
        src_layout.bytesPerRow = layout.bytes_per_row;
        src_layout.rowsPerImage = layout.rows_per_image;
        WGPUExtent3D write_size = {extent.width, extent.height, 1};
        wgpuQueueWriteTexture(queue_, &dst, data, size, &src_layout, &write_size);
    }

    [[nodiscard]] WGPUQueue raw() const { return queue_; }

private:
    WGPUQueue queue_;
};

class WgpuDevice final : public IDevice
{
public:
    WgpuDevice(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device)
        : instance_(instance), adapter_(adapter), device_(device),
          queue_(wgpuDeviceGetQueue(device))
    {
    }
    ~WgpuDevice() override
    {
        // The destructor body runs before member destructors: release device_ then adapter_
        // here; queue_ is released afterward by WgpuQueue's member destructor (wgpuQueueRelease).
        // wgpu-native objects are independently ref-counted, so this device->adapter->queue
        // release order is safe.
        if (device_ != nullptr)
        {
            wgpuDeviceRelease(device_);
        }
        if (adapter_ != nullptr)
        {
            wgpuAdapterRelease(adapter_);
        }
    }

    IQueue& queue() override { return queue_; }

    std::unique_ptr<ITexture> create_texture(const TextureDesc& desc) override
    {
        WGPUTextureDescriptor td{};
        td.label = sv("context-render-target");
        td.usage = 0;
        if (desc.render_attachment)
        {
            td.usage |= WGPUTextureUsage_RenderAttachment;
        }
        if (desc.copy_src)
        {
            td.usage |= WGPUTextureUsage_CopySrc;
        }
        if (desc.copy_dst)
        {
            td.usage |= WGPUTextureUsage_CopyDst;
        }
        if (desc.texture_binding)
        {
            td.usage |= WGPUTextureUsage_TextureBinding;
        }
        td.dimension = WGPUTextureDimension_2D;
        td.size = {desc.size.width, desc.size.height, 1};
        td.format = to_wgpu_format(desc.format);
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        return std::make_unique<WgpuTexture>(wgpuDeviceCreateTexture(device_, &td));
    }

    std::unique_ptr<IBuffer> create_buffer(const BufferDesc& desc) override
    {
        WGPUBufferDescriptor bd{};
        bd.label = sv("context-render-buffer");
        bd.usage = 0;
        if (desc.copy_dst)
        {
            bd.usage |= WGPUBufferUsage_CopyDst;
        }
        if (desc.map_read)
        {
            bd.usage |= WGPUBufferUsage_MapRead;
        }
        if (desc.uniform)
        {
            bd.usage |= WGPUBufferUsage_Uniform;
        }
        bd.size = desc.size;
        return std::make_unique<WgpuBuffer>(instance_, wgpuDeviceCreateBuffer(device_, &bd),
                                            desc.size);
    }

    std::unique_ptr<ISampler> create_sampler(const SamplerDesc& desc) override
    {
        WGPUSamplerDescriptor sd{};
        sd.label = sv("context-render-sampler");
        sd.addressModeU = to_wgpu_address(desc.address_mode);
        sd.addressModeV = to_wgpu_address(desc.address_mode);
        sd.addressModeW = to_wgpu_address(desc.address_mode);
        sd.magFilter = to_wgpu_filter(desc.mag_filter);
        sd.minFilter = to_wgpu_filter(desc.min_filter);
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.lodMinClamp = 0.0f;
        sd.lodMaxClamp = 32.0f;
        // != Undefined makes this a comparison sampler (the shadow-map PCF primitive).
        sd.compare = to_wgpu_compare(desc.compare);
        sd.maxAnisotropy = 1; // zero fails validation — 1 is the WebGPU default
        return std::make_unique<WgpuSampler>(wgpuDeviceCreateSampler(device_, &sd));
    }

    std::unique_ptr<IBindGroup> create_bind_group(IBindGroupLayout& layout,
                                                  const std::vector<BindGroupEntry>& entries) override
    {
        std::vector<WGPUBindGroupEntry> raw_entries;
        raw_entries.reserve(entries.size());
        for (const BindGroupEntry& e : entries)
        {
            WGPUBindGroupEntry entry{};
            entry.binding = e.binding;
            if (e.buffer != nullptr)
            {
                entry.buffer = static_cast<WgpuBuffer*>(e.buffer)->raw();
                entry.offset = e.buffer_offset;
                entry.size = (e.buffer_size != 0) ? e.buffer_size : WGPU_WHOLE_SIZE;
            }
            else
            {
                entry.size = WGPU_WHOLE_SIZE; // the header's INIT default; ignored without a buffer
            }
            if (e.texture != nullptr)
            {
                entry.textureView = static_cast<WgpuTextureView*>(e.texture)->raw();
            }
            if (e.sampler != nullptr)
            {
                entry.sampler = static_cast<WgpuSampler*>(e.sampler)->raw();
            }
            raw_entries.push_back(entry);
        }
        WGPUBindGroupDescriptor bd{};
        bd.label = sv("context-render-bind-group");
        bd.layout = static_cast<WgpuBindGroupLayout&>(layout).raw();
        bd.entryCount = raw_entries.size();
        bd.entries = raw_entries.data();
        return std::make_unique<WgpuBindGroup>(wgpuDeviceCreateBindGroup(device_, &bd));
    }

    std::unique_ptr<IRenderPipeline> create_render_pipeline(const RenderPipelineDesc& desc) override
    {
        WGPUShaderSourceWGSL wgsl{};
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(desc.wgsl.c_str());
        WGPUShaderModuleDescriptor sm{};
        sm.nextInChain = &wgsl.chain;
        sm.label = sv("context-render-wgsl");
        WGPUShaderModule shader = wgpuDeviceCreateShaderModule(device_, &sm);

        WGPUColorTargetState target{};
        target.format = to_wgpu_format(desc.color_format);
        target.writeMask = WGPUColorWriteMask_All; // zero-init means "write nothing" — be explicit

        // Blending is OPT-IN: an absent BlendState leaves target.blend null, which is WebGPU's
        // "replace" default and exactly what every pre-e03 pipeline in this repo was built with.
        WGPUBlendState blend{};
        if (desc.blend.has_value())
        {
            blend.color = to_wgpu_blend_component(desc.blend->color);
            blend.alpha = to_wgpu_blend_component(desc.blend->alpha);
            target.blend = &blend;
        }

        WGPUFragmentState fragment{};
        fragment.module = shader;
        fragment.entryPoint = sv(desc.fragment_entry.c_str());
        fragment.targetCount = 1;
        fragment.targets = &target;

        WGPURenderPipelineDescriptor pd{};
        pd.label = sv("context-render-pipeline");
        pd.vertex.module = shader;
        pd.vertex.entryPoint = sv(desc.vertex_entry.c_str());
        pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        pd.primitive.frontFace = WGPUFrontFace_CCW;
        pd.primitive.cullMode = WGPUCullMode_None;
        pd.multisample.count = 1;
        pd.multisample.mask = 0xFFFFFFFFu;
        // An empty fragment entry = a depth-only pipeline (the shadow pass): no fragment stage,
        // no color targets (WebGPU allows a vertex-only pipeline when a depth attachment exists).
        pd.fragment = desc.fragment_entry.empty() ? nullptr : &fragment;

        WGPUDepthStencilState depth{};
        if (desc.depth.has_value())
        {
            depth.format = to_wgpu_format(desc.depth->format);
            depth.depthWriteEnabled =
                desc.depth->depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            depth.depthCompare = to_wgpu_compare(desc.depth->depth_compare);
            // No stencil on Depth32Float: Undefined face states (zero-init) default to Always/Keep;
            // the masks follow the WebGPU defaults.
            depth.stencilReadMask = 0xFFFFFFFFu;
            depth.stencilWriteMask = 0xFFFFFFFFu;
            pd.depthStencil = &depth;
        }

        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &pd);
        wgpuShaderModuleRelease(shader);
        return std::make_unique<WgpuRenderPipeline>(pipeline);
    }

    std::unique_ptr<ICommandEncoder> create_command_encoder() override
    {
        return std::make_unique<WgpuCommandEncoder>(
            wgpuDeviceCreateCommandEncoder(device_, nullptr));
    }

    // The OSR interop seam (rhi.h). See the per-platform notes there and in
    // context/render/present/osr_import.h: the DECISION of which source to ask for is made one
    // layer up; this only honours it or fails closed.
    ExternalTexture import_external_texture(const ExternalTextureDesc& desc) override
    {
        ExternalTexture out;
        if (desc.coded_size.width == 0 || desc.coded_size.height == 0)
        {
            out.diagnostic = "external-texture import: coded_size is empty";
            return out;
        }

        switch (desc.source)
        {
        case ExternalTextureSource::CpuBgra:
            break; // handled below — the always-available path
        case ExternalTextureSource::IOSurface:
#if defined(CTX_RENDER_HAS_METAL_INTEROP)
            break; // the destination texture is allocated below, then blitted into by refresh
#else
            out.diagnostic = "IOSurface import needs macOS + wgpu-native's native Metal accessors";
            return out;
#endif
        case ExternalTextureSource::D3D12SharedHandle:
            // Not a "not yet wired" stub: stock wgpu-native's C API exposes NO external-texture
            // import at all, and the owner rejected carrying a patched fork (2026-07-19). The
            // policy layer never asks for this today; if it ever does, say exactly why it cannot
            // be honoured rather than degrading silently.
            out.diagnostic = "wgpu-native's C API exposes no D3D12 shared-handle import; the "
                             "Windows accelerated path is deferred pending gfx-rs/wgpu-native#621";
            return out;
        case ExternalTextureSource::DmaBuf:
            out.diagnostic = "dmabuf import is not implemented (research-grade, ships OFF)";
            return out;
        }

        // Both remaining paths need a BGRA8 texture the composite pass can sample: the CPU path
        // uploads into it, the IOSurface path blits into it.
        TextureDesc td;
        td.size = desc.coded_size;
        td.format = TextureFormat::BGRA8Unorm;
        td.copy_dst = true;
        td.texture_binding = true;
        std::unique_ptr<ITexture> texture = create_texture(td);
        if (texture == nullptr)
        {
            out.diagnostic = "external-texture import: the destination texture could not be created";
            return out;
        }
        out.texture = std::move(texture);
        out.accelerated = desc.source != ExternalTextureSource::CpuBgra;
        return out;
    }

    std::string refresh_external_texture(ITexture& texture,
                                         const ExternalTextureDesc& desc) override
    {
        if (desc.source != ExternalTextureSource::IOSurface)
        {
            return "no accelerated refresh exists for this external-texture source";
        }
#if defined(CTX_RENDER_HAS_METAL_INTEROP)
        // wgpu-native's STOCK native accessors (upstream PR #557) — no fork involved. Each returns
        // a BORROWED pointer valid only while its wgpu object lives, so nothing here is released.
        void* mtl_device = wgpuDeviceGetNativeMetalDevice(device_);
        void* mtl_queue = wgpuQueueGetNativeMetalCommandQueue(queue_.raw());
        void* mtl_texture = wgpuTextureGetNativeMetalTexture(static_cast<WgpuTexture&>(texture).raw());
        return detail::blit_iosurface(mtl_device, mtl_queue, desc.handle, mtl_texture);
#else
        (void)texture;
        return "IOSurface refresh needs macOS + wgpu-native's native Metal accessors";
#endif
    }

    [[nodiscard]] WGPUDevice raw() const { return device_; }

private:
    WGPUInstance instance_;
    WGPUAdapter adapter_;
    WGPUDevice device_;
    WgpuQueue queue_;
};

// ------------------------------------------------------------------------ present path (native)
#if !defined(__EMSCRIPTEN__)

// A configured presentation chain over one surface. The current backbuffer is OWNED per frame:
// wgpuSurfaceGetCurrentTexture returns it with ownership, so it is released at the next acquire or
// at teardown — retaining it across frames deadlocks the swapchain.
class WgpuSwapchain final : public ISwapchain
{
public:
    WgpuSwapchain(WGPUSurface surface, WGPUDevice device, const SurfaceConfig& config)
        : surface_(surface), device_(device), config_(config)
    {
        // RETAIN both. ISurface::configure hands back an independently-owned ISwapchain, and nothing
        // in the rhi.h contract obliges a caller to outlive it with the surface or device — so
        // borrowing raw handles here would let a legal `surface.reset(); chain->present();` call
        // into freed memory. Ref-counting is what wgpu-native's model is for, and it matches every
        // other wrapper in this file.
        wgpuSurfaceAddRef(surface_);
        wgpuDeviceAddRef(device_);
        apply_configuration();
    }

    WgpuSwapchain(const WgpuSwapchain&) = delete;
    WgpuSwapchain& operator=(const WgpuSwapchain&) = delete;

    ~WgpuSwapchain() override
    {
        release_frame();
        wgpuDeviceRelease(device_);
        wgpuSurfaceRelease(surface_);
    }

    [[nodiscard]] Extent2D size() const override { return config_.size; }
    [[nodiscard]] TextureFormat format() const override { return config_.format; }
    [[nodiscard]] PresentMode present_mode() const override { return config_.present_mode; }
    [[nodiscard]] bool is_configured() const override { return configured_; }

    AcquiredFrame acquire() override
    {
        AcquiredFrame frame;
        if (!configured_)
        {
            return frame; // status stays Error
        }
        release_frame(); // drop the previous frame's texture/view before asking for the next

        WGPUSurfaceTexture current{};
        wgpuSurfaceGetCurrentTexture(surface_, &current);
        switch (current.status)
        {
        case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
            frame.status = AcquireStatus::Ok;
            break;
        case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
            // Presentable, just no longer ideally configured (a pending resize): present it, then
            // reconfigure. Treating this as Outdated would drop a perfectly good frame.
            frame.status = AcquireStatus::Ok;
            frame.suboptimal = true;
            break;
        case WGPUSurfaceGetCurrentTextureStatus_Timeout:
            frame.status = AcquireStatus::Timeout;
            break;
        case WGPUSurfaceGetCurrentTextureStatus_Outdated:
            frame.status = AcquireStatus::Outdated;
            break;
        case WGPUSurfaceGetCurrentTextureStatus_Lost:
            frame.status = AcquireStatus::Lost;
            break;
        default:
            frame.status = AcquireStatus::Error;
            break;
        }

        if (frame.status != AcquireStatus::Ok)
        {
            // A non-Ok status can still hand back a texture; release it so it is not leaked.
            if (current.texture != nullptr)
            {
                wgpuTextureRelease(current.texture);
            }
            return frame;
        }

        WGPUTextureView raw_view = wgpuTextureCreateView(current.texture, nullptr);
        if (raw_view == nullptr)
        {
            // Reporting Ok with a null view would defer the failure into the render pass, where it
            // reads as a driver bug rather than a failed acquire.
            wgpuTextureRelease(current.texture);
            frame.status = AcquireStatus::Error;
            frame.view = nullptr;
            return frame;
        }

        current_texture_ = current.texture;
        view_ = std::make_unique<WgpuTextureView>(raw_view);
        frame.view = view_.get();
        acquired_ = true;
        return frame;
    }

    void present() override
    {
        if (!acquired_)
        {
            return;
        }
        acquired_ = false;
        wgpuSurfacePresent(surface_);
    }

    void resize(Extent2D size) override
    {
        if (size.width == 0 || size.height == 0)
        {
            return; // a minimized window keeps its last valid configuration
        }
        release_frame();
        config_.size = size;
        apply_configuration();
    }

    void unconfigure() override
    {
        release_frame();
        if (configured_)
        {
            wgpuSurfaceUnconfigure(surface_);
            configured_ = false;
        }
    }

private:
    void apply_configuration()
    {
        WGPUSurfaceConfiguration cfg{};
        cfg.device = device_;
        cfg.format = to_wgpu_format(config_.format);
        cfg.usage = WGPUTextureUsage_RenderAttachment;
        cfg.width = config_.size.width;
        cfg.height = config_.size.height;
        cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
        cfg.presentMode = to_wgpu_present_mode(config_.present_mode);
        wgpuSurfaceConfigure(surface_, &cfg);
        configured_ = true;
    }

    void release_frame()
    {
        view_.reset();
        if (current_texture_ != nullptr)
        {
            wgpuTextureRelease(current_texture_);
            current_texture_ = nullptr;
        }
        acquired_ = false;
    }

    WGPUSurface surface_;
    WGPUDevice device_;
    SurfaceConfig config_;
    WGPUTexture current_texture_ = nullptr;
    std::unique_ptr<WgpuTextureView> view_;
    bool configured_ = false;
    bool acquired_ = false;
};

class WgpuSurface final : public ISurface
{
public:
    WgpuSurface(WGPUSurface surface, WGPUAdapter adapter) : surface_(surface), adapter_(adapter) {}

    ~WgpuSurface() override
    {
        if (surface_ != nullptr)
        {
            wgpuSurfaceRelease(surface_);
        }
        if (adapter_ != nullptr)
        {
            wgpuAdapterRelease(adapter_);
        }
    }

    SurfaceCaps capabilities() override
    {
        SurfaceCaps caps;
        if (adapter_ == nullptr)
        {
            return caps; // supported stays false — a clean "cannot present here" report
        }
        WGPUSurfaceCapabilities raw{};
        if (wgpuSurfaceGetCapabilities(surface_, adapter_, &raw) != WGPUStatus_Success)
        {
            return caps;
        }
        for (std::size_t i = 0; i < raw.formatCount; ++i)
        {
            TextureFormat format{};
            if (from_wgpu_format(raw.formats[i], format))
            {
                caps.formats.push_back(format);
            }
        }
        for (std::size_t i = 0; i < raw.presentModeCount; ++i)
        {
            PresentMode mode{};
            if (from_wgpu_present_mode(raw.presentModes[i], mode))
            {
                caps.present_modes.push_back(mode);
            }
        }
        wgpuSurfaceCapabilitiesFreeMembers(raw);
        // "Supported" means there is at least one format AND one present mode we can actually name;
        // an adapter that reports only formats outside the T1 set cannot present for our purposes.
        caps.supported = !caps.formats.empty() && !caps.present_modes.empty();
        return caps;
    }

    std::unique_ptr<ISwapchain> configure(IDevice& device, const SurfaceConfig& config) override
    {
        if (config.size.width == 0 || config.size.height == 0)
        {
            return nullptr;
        }
        // Capability-check BEFORE configuring (the spike's proven order, main.cpp:546-560): a
        // configure with an unsupported format/mode is a validation error, not a graceful refusal.
        const SurfaceCaps caps = capabilities();
        if (!caps.supported || !caps.supports_format(config.format) ||
            !caps.supports_present_mode(config.present_mode))
        {
            return nullptr;
        }
        return std::make_unique<WgpuSwapchain>(surface_, static_cast<WgpuDevice&>(device).raw(),
                                               config);
    }

private:
    WGPUSurface surface_;
    WGPUAdapter adapter_; // the adapter capabilities are reported against (owned)
};

#endif // !__EMSCRIPTEN__

class WgpuRhi final : public IRhi
{
public:
    explicit WgpuRhi(WGPUInstance instance) : instance_(instance) {}
    ~WgpuRhi() override
    {
        if (instance_ != nullptr)
        {
            wgpuInstanceRelease(instance_);
        }
    }

    [[nodiscard]] RhiTier tier() const override { return RhiTier::T1_WebGPU; }
    [[nodiscard]] const char* backend_name() const override
    {
#if defined(__EMSCRIPTEN__)
        return "browser-webgpu"; // emdawnwebgpu -> navigator.gpu (the web T1 backend, issue #137).
#else
        return "wgpu-native";
#endif
    }

    AdapterProbe probe() override
    {
        AdapterProbe out;
#if defined(CTX_RENDER_HAS_WGPU_EXTRAS)
        // Enumerate adapters WITHOUT creating a device (R-HEAD-002).
        const std::size_t count = wgpuInstanceEnumerateAdapters(instance_, nullptr, nullptr);
        out.adapter_count = count;
        out.has_adapter = count > 0;
        if (count > 0)
        {
            std::vector<WGPUAdapter> adapters(count);
            wgpuInstanceEnumerateAdapters(instance_, nullptr, adapters.data());
            WGPUAdapterInfo info{};
            if (wgpuAdapterGetInfo(adapters[0], &info) == WGPUStatus_Success)
            {
                out.primary_name = sv_to_string(info.device);
                wgpuAdapterInfoFreeMembers(info);
            }
            for (WGPUAdapter a : adapters)
            {
                wgpuAdapterRelease(a);
            }
        }
#else
        // No enumerate extra: fall back to requestAdapter (still no device created).
        std::string failure;
        WGPUAdapter adapter = request_adapter(instance_, &failure);
        out.has_adapter = adapter != nullptr;
        out.adapter_count = out.has_adapter ? 1u : 0u;
        if (adapter != nullptr)
        {
            wgpuAdapterRelease(adapter);
        }
#endif
        return out;
    }

    std::unique_ptr<IDevice> create_device() override
    {
        std::string failure;
        WGPUAdapter adapter = request_adapter(instance_, &failure);
        if (adapter == nullptr)
        {
            return nullptr;
        }
        WGPUDevice device = request_device(instance_, adapter);
        if (device == nullptr)
        {
            wgpuAdapterRelease(adapter);
            return nullptr;
        }
        return std::make_unique<WgpuDevice>(instance_, adapter, device);
    }

    std::unique_ptr<ISurface> create_surface(const NativeWindowDesc& window) override
    {
#if defined(__EMSCRIPTEN__)
        // The web path presents to a canvas, not a native window; that surface source is a later
        // wave. Reporting a clean absence keeps the web build honest instead of half-wired.
        (void)window;
        return nullptr;
#else
        if (window.kind == NativeWindowKind::None || window.handle == nullptr)
        {
            return nullptr;
        }

        WGPUSurfaceDescriptor sd{};
        sd.label = sv("context-render-surface");

        // Exactly one per-OS source struct is chained, selected by the descriptor's kind. Each is
        // declared in the widest scope so its address stays valid through the create call.
        WGPUSurfaceSourceWindowsHWND hwnd_source{};
        WGPUSurfaceSourceXlibWindow xlib_source{};
        WGPUSurfaceSourceWaylandSurface wayland_source{};
        WGPUSurfaceSourceMetalLayer metal_source{};

        switch (window.kind)
        {
        case NativeWindowKind::Win32Hwnd:
            hwnd_source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
            hwnd_source.hinstance = window.display;
            hwnd_source.hwnd = window.handle;
            sd.nextInChain = &hwnd_source.chain;
            break;
        case NativeWindowKind::XlibWindow:
            xlib_source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
            xlib_source.display = window.display;
            // An X11 Window is an XID, carried through the void* handle slot.
            xlib_source.window = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(window.handle));
            sd.nextInChain = &xlib_source.chain;
            break;
        case NativeWindowKind::WaylandSurface:
            wayland_source.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
            wayland_source.display = window.display;
            wayland_source.surface = window.handle;
            sd.nextInChain = &wayland_source.chain;
            break;
        case NativeWindowKind::MetalLayer:
            metal_source.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
            metal_source.layer = window.handle;
            sd.nextInChain = &metal_source.chain;
            break;
        case NativeWindowKind::None:
        default:
            return nullptr;
        }

        WGPUSurface surface = wgpuInstanceCreateSurface(instance_, &sd);
        if (surface == nullptr)
        {
            return nullptr;
        }
        // Capabilities are reported for a (surface, adapter) PAIR, so the surface holds the adapter
        // it was probed against — requested WITH compatibleSurface set, which is what makes the
        // answer meaningful on a multi-GPU box.
        // A null adapter here is a GPU-less or unpresentable box: reported by WgpuSurface's
        // capability report, never fatal.
        return std::make_unique<WgpuSurface>(surface, request_adapter(instance_, nullptr, surface));
#endif
    }

    AdapterProbe probe_surface(ISurface& surface) override
    {
        AdapterProbe result = probe();
#if defined(__EMSCRIPTEN__)
        (void)surface;
#else
        // Still device-free (R-HEAD-002): presentability is answered from the surface capabilities.
        result.can_present = result.has_adapter && surface.capabilities().supported;
#endif
        return result;
    }

private:
    WGPUInstance instance_;
};

} // namespace

std::unique_ptr<IRhi> create_wgpu_rhi()
{
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (instance == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<WgpuRhi>(instance);
}

} // namespace context::render
