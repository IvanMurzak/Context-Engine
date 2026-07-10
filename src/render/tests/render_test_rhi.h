// A GPU-free, header-only fake RHI backend for the render tests. It implements the SAME rhi.h
// interface the wgpu-native backend implements, so a test can drive the full object model
// (device/queue/pipeline/buffer/texture/command-encoding) end to end with no GPU — and the fake
// software-rasterizes the SAME triangle the real WGSL shader draws, so the pixel-readback assertions
// here match the ones the real offscreen proof (context/render/wgpu) asserts against a real adapter.

#pragma once

#include "context/render/rhi.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace rendertest
{

using namespace context::render;

// Convert a WebGPU-style float clear component [0,1] to an 8-bit unorm (round-to-nearest).
inline std::uint8_t unorm8(double c)
{
    if (c < 0.0)
    {
        c = 0.0;
    }
    if (c > 1.0)
    {
        c = 1.0;
    }
    return static_cast<std::uint8_t>(c * 255.0 + 0.5);
}

// Software-rasterize the reference triangle into an RGBA8 image (row-major, width*height*4 bytes),
// matching the WGSL shader in the offscreen proof: clip-space vertices (0,0.5)/(-0.5,-0.5)/(0.5,-0.5)
// mapped to screen (128,64)/(64,192)/(192,192) at 256x256, fragment solid red, background = clear.
inline void rasterize_reference_triangle(std::vector<std::uint8_t>& image, std::uint32_t width,
                                         std::uint32_t height, const Color& clear)
{
    image.assign(static_cast<std::size_t>(width) * height * 4u, 0u);
    const std::uint8_t br = unorm8(clear.r);
    const std::uint8_t bg = unorm8(clear.g);
    const std::uint8_t bb = unorm8(clear.b);
    const std::uint8_t ba = unorm8(clear.a);

    // Screen-space triangle vertices (NDC y-up -> screen y-down).
    const double v0x = 0.5 * width, v0y = 0.25 * height;  // (128, 64)
    const double v1x = 0.25 * width, v1y = 0.75 * height; // (64, 192)
    const double v2x = 0.75 * width, v2y = 0.75 * height; // (192, 192)

    auto edge = [](double ax, double ay, double bx, double by, double px, double py)
    { return (px - ax) * (by - ay) - (py - ay) * (bx - ax); };

    // Winding sign of the whole triangle, so the inside test works regardless of vertex order.
    const double area = edge(v0x, v0y, v1x, v1y, v2x, v2y);
    const double sign = area >= 0.0 ? 1.0 : -1.0;

    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const double px = x + 0.5;
            const double py = y + 0.5;
            const double w0 = edge(v0x, v0y, v1x, v1y, px, py) * sign;
            const double w1 = edge(v1x, v1y, v2x, v2y, px, py) * sign;
            const double w2 = edge(v2x, v2y, v0x, v0y, px, py) * sign;
            std::uint8_t* p = image.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
            if (w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0)
            {
                p[0] = 255;
                p[1] = 0;
                p[2] = 0;
                p[3] = 255;
            }
            else
            {
                p[0] = br;
                p[1] = bg;
                p[2] = bb;
                p[3] = ba;
            }
        }
    }
}

// ------------------------------------------------------------------------------- fake RHI objects

class FakeTextureView : public ITextureView
{
public:
    explicit FakeTextureView(class FakeTexture* owner) : owner_(owner) {}
    [[nodiscard]] FakeTexture* owner() const { return owner_; }

private:
    FakeTexture* owner_;
};

class FakeTexture : public ITexture
{
public:
    FakeTexture(Extent2D size, TextureFormat format) : size_(size), format_(format)
    {
        pixels_.assign(static_cast<std::size_t>(size.width) * size.height * 4u, 0u);
    }

    std::unique_ptr<ITextureView> create_view() override
    {
        return std::make_unique<FakeTextureView>(this);
    }

    [[nodiscard]] Extent2D size() const { return size_; }
    [[nodiscard]] TextureFormat format() const { return format_; }
    [[nodiscard]] std::vector<std::uint8_t>& pixels() { return pixels_; }

private:
    Extent2D size_;
    TextureFormat format_;
    std::vector<std::uint8_t> pixels_;
};

class FakeBuffer : public IBuffer
{
public:
    explicit FakeBuffer(std::uint64_t size)
        : bytes_(static_cast<std::size_t>(size), 0u), mapped_(false)
    {
    }

    [[nodiscard]] std::uint64_t size() const override { return bytes_.size(); }

    const void* map_read() override
    {
        mapped_ = true;
        return bytes_.data();
    }

    void unmap() override { mapped_ = false; }

    [[nodiscard]] std::vector<std::uint8_t>& bytes() { return bytes_; }
    [[nodiscard]] bool is_mapped() const { return mapped_; }

private:
    std::vector<std::uint8_t> bytes_;
    bool mapped_;
};

class FakeSampler : public ISampler
{
public:
    explicit FakeSampler(const SamplerDesc& desc) : desc_(desc) {}
    [[nodiscard]] const SamplerDesc& desc() const { return desc_; }

private:
    SamplerDesc desc_;
};

class FakeBindGroupLayout : public IBindGroupLayout
{
public:
    explicit FakeBindGroupLayout(std::uint32_t group) : group_(group) {}
    [[nodiscard]] std::uint32_t group() const { return group_; }

private:
    std::uint32_t group_;
};

class FakeBindGroup : public IBindGroup
{
public:
    FakeBindGroup(std::uint32_t group, std::vector<BindGroupEntry> entries)
        : group_(group), entries_(std::move(entries))
    {
    }
    [[nodiscard]] std::uint32_t group() const { return group_; }
    [[nodiscard]] const std::vector<BindGroupEntry>& entries() const { return entries_; }

private:
    std::uint32_t group_;
    std::vector<BindGroupEntry> entries_;
};

class FakePipeline : public IRenderPipeline
{
public:
    explicit FakePipeline(const RenderPipelineDesc& desc) : desc_(desc) {}

    std::unique_ptr<IBindGroupLayout> bind_group_layout(std::uint32_t group) override
    {
        return std::make_unique<FakeBindGroupLayout>(group);
    }

    [[nodiscard]] const RenderPipelineDesc& desc() const { return desc_; }

private:
    RenderPipelineDesc desc_;
};

class FakeCommandBuffer : public ICommandBuffer
{
};

class FakeRenderPassEncoder : public IRenderPassEncoder
{
public:
    FakeRenderPassEncoder(FakeTexture* target, Color clear, LoadOp load)
        : target_(target), clear_(clear), load_(load)
    {
    }

    void set_pipeline(IRenderPipeline& /*pipeline*/) override { pipeline_set_ = true; }

    void set_bind_group(std::uint32_t /*index*/, IBindGroup& /*group*/) override
    {
        ++bind_group_sets_;
    }

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count) override
    {
        draw_vertices_ = vertex_count;
        draw_instances_ = instance_count;
    }

    [[nodiscard]] int bind_group_sets() const { return bind_group_sets_; }

    void end() override
    {
        if (target_ == nullptr)
        {
            return;
        }
        // Apply the pass: clear the target, then rasterize the reference triangle if a draw was
        // recorded (a 3-vertex TriangleList). Mirrors the real backend's clear+draw.
        const Extent2D size = target_->size();
        if (load_ == LoadOp::Clear)
        {
            if (pipeline_set_ && draw_vertices_ == 3)
            {
                rasterize_reference_triangle(target_->pixels(), size.width, size.height, clear_);
            }
            else
            {
                std::vector<std::uint8_t>& px = target_->pixels();
                for (std::size_t i = 0; i + 3 < px.size(); i += 4)
                {
                    px[i + 0] = unorm8(clear_.r);
                    px[i + 1] = unorm8(clear_.g);
                    px[i + 2] = unorm8(clear_.b);
                    px[i + 3] = unorm8(clear_.a);
                }
            }
        }
    }

private:
    FakeTexture* target_;
    Color clear_;
    LoadOp load_;
    bool pipeline_set_ = false;
    int bind_group_sets_ = 0;
    std::uint32_t draw_vertices_ = 0;
    std::uint32_t draw_instances_ = 0;
};

class FakeCommandEncoder : public ICommandEncoder
{
public:
    std::unique_ptr<IRenderPassEncoder> begin_render_pass(const RenderPassDesc& desc) override
    {
        FakeTexture* target = nullptr;
        Color clear;
        LoadOp load = LoadOp::Clear;
        if (!desc.color.empty() && desc.color[0].view != nullptr)
        {
            auto* view = static_cast<FakeTextureView*>(desc.color[0].view);
            target = view->owner();
            clear = desc.color[0].clear;
            load = desc.color[0].load;
        }
        return std::make_unique<FakeRenderPassEncoder>(target, clear, load);
    }

    void copy_texture_to_buffer(ITexture& src, IBuffer& dst, const TexelCopyBufferLayout& layout,
                                Extent2D extent) override
    {
        auto& texture = static_cast<FakeTexture&>(src);
        auto& buffer = static_cast<FakeBuffer&>(dst);
        const std::vector<std::uint8_t>& tex = texture.pixels();
        std::vector<std::uint8_t>& buf = buffer.bytes();
        const std::uint32_t src_bpr = extent.width * 4u;
        for (std::uint32_t row = 0; row < extent.height; ++row)
        {
            const std::size_t src_off = static_cast<std::size_t>(row) * src_bpr;
            const std::size_t dst_off = static_cast<std::size_t>(row) * layout.bytes_per_row;
            if (dst_off + src_bpr <= buf.size() && src_off + src_bpr <= tex.size())
            {
                std::memcpy(buf.data() + dst_off, tex.data() + src_off, src_bpr);
            }
        }
    }

    std::unique_ptr<ICommandBuffer> finish() override
    {
        return std::make_unique<FakeCommandBuffer>();
    }
};

class FakeQueue : public IQueue
{
public:
    void submit(ICommandBuffer& /*commands*/) override { ++submit_count_; }

    void write_buffer(IBuffer& buffer, std::uint64_t offset, const void* data,
                      std::size_t size) override
    {
        auto& fake = static_cast<FakeBuffer&>(buffer);
        if (offset + size <= fake.bytes().size())
        {
            std::memcpy(fake.bytes().data() + offset, data, size);
        }
    }

    void write_texture(ITexture& texture, const void* data, std::size_t size,
                       const TexelCopyBufferLayout& layout, Extent2D extent) override
    {
        auto& fake = static_cast<FakeTexture&>(texture);
        const auto* src = static_cast<const std::uint8_t*>(data);
        const std::uint32_t row_bytes = extent.width * 4u;
        for (std::uint32_t row = 0; row < extent.height; ++row)
        {
            const std::size_t src_off = static_cast<std::size_t>(row) * layout.bytes_per_row;
            const std::size_t dst_off = static_cast<std::size_t>(row) * row_bytes;
            if (src_off + row_bytes <= size && dst_off + row_bytes <= fake.pixels().size())
            {
                std::memcpy(fake.pixels().data() + dst_off, src + src_off, row_bytes);
            }
        }
    }

    [[nodiscard]] int submit_count() const { return submit_count_; }

private:
    int submit_count_ = 0;
};

class FakeDevice : public IDevice
{
public:
    IQueue& queue() override { return queue_; }

    std::unique_ptr<ITexture> create_texture(const TextureDesc& desc) override
    {
        return std::make_unique<FakeTexture>(desc.size, desc.format);
    }

    std::unique_ptr<IBuffer> create_buffer(const BufferDesc& desc) override
    {
        return std::make_unique<FakeBuffer>(desc.size);
    }

    std::unique_ptr<ISampler> create_sampler(const SamplerDesc& desc) override
    {
        return std::make_unique<FakeSampler>(desc);
    }

    std::unique_ptr<IRenderPipeline> create_render_pipeline(const RenderPipelineDesc& desc) override
    {
        return std::make_unique<FakePipeline>(desc);
    }

    std::unique_ptr<IBindGroup> create_bind_group(IBindGroupLayout& layout,
                                                  const std::vector<BindGroupEntry>& entries) override
    {
        auto& fake_layout = static_cast<FakeBindGroupLayout&>(layout);
        return std::make_unique<FakeBindGroup>(fake_layout.group(), entries);
    }

    std::unique_ptr<ICommandEncoder> create_command_encoder() override
    {
        return std::make_unique<FakeCommandEncoder>();
    }

private:
    FakeQueue queue_;
};

// The fake factory. adapter_count controls the probe result; device_creations counts how many
// devices were made — a test asserts probe() creates NONE (R-HEAD-002).
class FakeRhi : public IRhi
{
public:
    explicit FakeRhi(std::size_t adapter_count) : adapter_count_(adapter_count) {}

    [[nodiscard]] RhiTier tier() const override { return RhiTier::T1_WebGPU; }
    [[nodiscard]] const char* backend_name() const override { return "fake"; }

    AdapterProbe probe() override
    {
        AdapterProbe result;
        result.adapter_count = adapter_count_;
        result.has_adapter = adapter_count_ > 0;
        result.primary_name = result.has_adapter ? "fake-adapter" : "";
        return result;
    }

    std::unique_ptr<IDevice> create_device() override
    {
        if (adapter_count_ == 0)
        {
            return nullptr;
        }
        ++device_creations_;
        return std::make_unique<FakeDevice>();
    }

    [[nodiscard]] int device_creations() const { return device_creations_; }

private:
    std::size_t adapter_count_;
    int device_creations_ = 0;
};

} // namespace rendertest
