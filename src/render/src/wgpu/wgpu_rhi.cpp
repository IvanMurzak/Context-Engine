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

WGPUAdapter request_adapter(WGPUInstance instance, std::string* failure)
{
    WGPURequestAdapterOptions opts{};
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

    void write_texture(ITexture& texture, const void* data, std::size_t size,
                       const TexelCopyBufferLayout& layout, Extent2D extent) override
    {
        WGPUTexelCopyTextureInfo dst{};
        dst.texture = static_cast<WgpuTexture&>(texture).raw();
        WGPUTexelCopyBufferLayout src_layout{};
        src_layout.bytesPerRow = layout.bytes_per_row;
        src_layout.rowsPerImage = layout.rows_per_image;
        WGPUExtent3D write_size = {extent.width, extent.height, 1};
        wgpuQueueWriteTexture(queue_, &dst, data, size, &src_layout, &write_size);
    }

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

private:
    WGPUInstance instance_;
    WGPUAdapter adapter_;
    WGPUDevice device_;
    WgpuQueue queue_;
};

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
