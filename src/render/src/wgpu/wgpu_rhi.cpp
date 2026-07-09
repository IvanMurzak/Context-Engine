// The T1 native RHI backend over wgpu-native (R-REND-001/002) — see wgpu/wgpu_rhi.h.
//
// Implements the rhi.h object model over webgpu.h, faithfully reusing the wgpu calls proven by the
// throwaway spikes/webgpu spike (adapter/device request + pump loop, offscreen texture + readback
// buffer, WGSL pipeline, render pass, texture->buffer copy, async map). This TU is the ONLY place
// webgpu.h is included; it is compiled solely under CONTEXT_BUILD_RENDER_WGPU (a CI-gated dependency
// path, like V8) because the sole off-the-shelf Windows prebuilt is MSVC-ABI. It is platform-neutral
// (no window/surface code — offscreen only) so it has no #if platform branches.

#include "context/render/wgpu/wgpu_rhi.h"

#include <webgpu/webgpu.h>

#if defined(CTX_RENDER_HAS_WGPU_EXTRAS)
#include <webgpu/wgpu.h> // wgpuInstanceEnumerateAdapters — the R-HEAD-002 device-free probe.
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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

// One poll step: deliver completed async events, then yield briefly (native path).
void pump(WGPUInstance instance)
{
    wgpuInstanceProcessEvents(instance);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
        bd.size = desc.size;
        return std::make_unique<WgpuBuffer>(instance_, wgpuDeviceCreateBuffer(device_, &bd),
                                            desc.size);
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
        pd.fragment = &fragment;

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
    [[nodiscard]] const char* backend_name() const override { return "wgpu-native"; }

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
