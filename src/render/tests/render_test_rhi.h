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
#include <set>
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

// A DEVICE-LIFETIME record of what the render passes did. A pass encoder is created and destroyed
// inside one frame, so per-encoder counters are unreachable by the time a test asserts; the multi-pass
// compositor (e04: a viewport pass, the full-window CEF pass, then the scissored PET_POPUP pass) is
// exactly the caller that needs the aggregate. Owned by FakeDevice and threaded down.
struct FakePassLog
{
    int passes = 0;
    int draws = 0;
    int scissor_sets = 0;
    // Every scissor rect in call order — the popup layer's confinement is asserted from this.
    std::vector<Rect2D> scissors;
    // Every uniform-buffer payload written, in call order. The per-layer composite UV is otherwise
    // UNOBSERVABLE: the compositor uploads it to a uniform buffer it owns privately, and a wrong UV
    // renders a layer squeezed or offset rather than tripping any counter here. Recording the bytes
    // is what lets a headless test assert the UV arithmetic the GPU would have sampled with.
    std::vector<std::vector<std::uint8_t>> buffer_writes;
};

class FakeRenderPassEncoder : public IRenderPassEncoder
{
public:
    FakeRenderPassEncoder(FakeTexture* target, Color clear, LoadOp load, FakePassLog* log = nullptr)
        : target_(target), clear_(clear), load_(load), log_(log)
    {
        if (log_ != nullptr)
        {
            ++log_->passes;
        }
    }

    void set_pipeline(IRenderPipeline& /*pipeline*/) override { pipeline_set_ = true; }

    void set_bind_group(std::uint32_t /*index*/, IBindGroup& /*group*/) override
    {
        ++bind_group_sets_;
    }

    // The scissor is RECORDED rather than applied: the fake only rasterizes the reference triangle.
    // What a headless test can honestly assert is that the compositor CONFINED the popup draw —
    // i.e. the rect it passed — and, via FakePassLog::buffer_writes, the UV it paired with it.
    //
    // NO real-adapter proof of the scissor exists yet. `render-wgpu-osr-composite` is e03's
    // full-window OSR composite gate; it predates set_scissor_rect and never scissors, so the wgpu
    // implementation (wgpuRenderPassEncoderSetScissorRect) is currently executed by no CI leg. The
    // per-pixel proof belongs in the wgpu offscreen harness — a scissored sub-rect draw asserting
    // that pixels outside the rect are untouched — and lands with the first task that needs it.
    void set_scissor_rect(Origin2D origin, Extent2D size) override
    {
        last_scissor_ = Rect2D{origin, size};
        if (log_ != nullptr)
        {
            ++log_->scissor_sets;
            log_->scissors.push_back(last_scissor_);
        }
    }

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count) override
    {
        draw_vertices_ = vertex_count;
        draw_instances_ = instance_count;
        if (log_ != nullptr)
        {
            ++log_->draws;
        }
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
    FakePassLog* log_ = nullptr;
    bool pipeline_set_ = false;
    int bind_group_sets_ = 0;
    Rect2D last_scissor_{};
    std::uint32_t draw_vertices_ = 0;
    std::uint32_t draw_instances_ = 0;
};

class FakeCommandEncoder : public ICommandEncoder
{
public:
    explicit FakeCommandEncoder(FakePassLog* log = nullptr) : log_(log) {}

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
        return std::make_unique<FakeRenderPassEncoder>(target, clear, load, log_);
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

private:
    FakePassLog* log_ = nullptr;
};

class FakeQueue : public IQueue
{
public:
    void submit(ICommandBuffer& /*commands*/) override { ++submit_count_; }

    void set_log(FakePassLog* log) { log_ = log; }

    void write_buffer(IBuffer& buffer, std::uint64_t offset, const void* data,
                      std::size_t size) override
    {
        auto& fake = static_cast<FakeBuffer&>(buffer);
        if (offset + size <= fake.bytes().size())
        {
            std::memcpy(fake.bytes().data() + offset, data, size);
        }
        if (log_ != nullptr && data != nullptr)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            log_->buffer_writes.emplace_back(bytes, bytes + size);
        }
    }

    void write_texture_region(ITexture& texture, Origin2D origin, const void* data,
                              std::size_t size, const TexelCopyBufferLayout& layout,
                              Extent2D extent) override
    {
        auto& fake = static_cast<FakeTexture&>(texture);
        const auto* src = static_cast<const std::uint8_t*>(data);
        const std::uint32_t dst_row_bytes = fake.size().width * 4u;
        const std::uint32_t row_bytes = extent.width * 4u;
        written_texels_ += static_cast<std::uint64_t>(extent.width) * extent.height;
        for (std::uint32_t row = 0; row < extent.height; ++row)
        {
            const std::size_t src_off = static_cast<std::size_t>(row) * layout.bytes_per_row;
            // The destination row is offset by the sub-rect's origin — this is what makes a dirty
            // rect land where it belongs instead of at the texture's top-left corner.
            const std::size_t dst_off =
                (static_cast<std::size_t>(origin.y + row) * fake.size().width + origin.x) * 4u;
            if (src_off + row_bytes <= size && dst_off + row_bytes <= fake.pixels().size() &&
                origin.x * 4u + row_bytes <= dst_row_bytes)
            {
                std::memcpy(fake.pixels().data() + dst_off, src + src_off, row_bytes);
            }
        }
    }

    [[nodiscard]] int submit_count() const { return submit_count_; }
    // How many sub-rect uploads were issued, and how many texels they moved in total — the
    // dirty-rect path's whole point is that both stay far below a full-frame re-upload.
    [[nodiscard]] std::uint64_t written_texels() const { return written_texels_; }

private:
    int submit_count_ = 0;
    std::uint64_t written_texels_ = 0;
    FakePassLog* log_ = nullptr;
};

class FakeDevice : public IDevice
{
public:
    // The queue shares the device-lifetime pass log, so a uniform upload and the draw it feeds are
    // recorded against the same timeline and can be correlated by index.
    FakeDevice() { queue_.set_log(&pass_log_); }

    // Non-copyable BECAUSE of that self-reference: a copy's queue would keep logging into the
    // DONOR's pass log, and the bug would surface as a test asserting against another test's draws.
    FakeDevice(const FakeDevice&) = delete;
    FakeDevice& operator=(const FakeDevice&) = delete;

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
        return std::make_unique<FakeCommandEncoder>(&pass_log_);
    }

    // What every render pass this device recorded actually did — see FakePassLog.
    [[nodiscard]] const FakePassLog& pass_log() const { return pass_log_; }

    // Mirrors the real backend's import policy WITHOUT a GPU: CpuBgra always succeeds (it is just an
    // upload-destination texture); an accelerated source succeeds only when the test declared it
    // available, and otherwise FAILS CLOSED exactly as wgpu_rhi.cpp does — which is what lets the
    // import-policy tests drive the Windows (no accelerated path) and macOS (accelerated) branches
    // on every OS, including this GPU-less one.
    ExternalTexture import_external_texture(const ExternalTextureDesc& desc) override
    {
        ExternalTexture out;
        if (import_always_fails_)
        {
            // Even the CpuBgra path can fail on a real backend (an exhausted device, a lost
            // device). Without this knob the importer's outright-failure branch is unreachable,
            // leaving the seam's primary fail-closed contract untested.
            out.diagnostic = "fake backend: texture allocation refused";
            return out;
        }
        if (desc.source == ExternalTextureSource::CpuBgra)
        {
            out.texture = std::make_unique<FakeTexture>(desc.coded_size, TextureFormat::BGRA8Unorm);
            out.accelerated = false;
            return out;
        }
        if (available_accelerated_.count(static_cast<int>(desc.source)) == 0)
        {
            out.diagnostic = "fake backend: accelerated external-texture source unavailable";
            return out; // texture stays null — fail closed
        }
        out.texture = std::make_unique<FakeTexture>(desc.coded_size, TextureFormat::BGRA8Unorm);
        out.accelerated = true;
        return out;
    }

    std::string refresh_external_texture(ITexture& /*texture*/,
                                         const ExternalTextureDesc& desc) override
    {
        if (desc.source == ExternalTextureSource::CpuBgra)
        {
            return "the CPU upload path refreshes through write_texture_region, not refresh";
        }
        if (available_accelerated_.count(static_cast<int>(desc.source)) == 0)
        {
            return "fake backend: accelerated external-texture source unavailable";
        }
        if (desc.handle == nullptr)
        {
            return "fake backend: the accelerated refresh needs the producer's frame handle";
        }
        ++refresh_count_;
        return {};
    }

    // Declare an accelerated source as available on this fake device (test scaffolding).
    void set_accelerated_available(ExternalTextureSource source)
    {
        available_accelerated_.insert(static_cast<int>(source));
    }
    // Make EVERY import fail, including CpuBgra — the "no texture at all" case (device exhausted or
    // lost) that no source-specific knob can produce.
    void set_import_always_fails(bool fails) { import_always_fails_ = fails; }
    [[nodiscard]] int refresh_count() const { return refresh_count_; }

private:
    FakeQueue queue_;
    FakePassLog pass_log_;
    std::set<int> available_accelerated_;
    int refresh_count_ = 0;
    bool import_always_fails_ = false;
};

// ------------------------------------------------------------------------- fake present path
// A GPU-free ISwapchain that models the real acquire/present/resize/unconfigure STATE MACHINE, so
// the compositor's frame loop — including the Outdated/Lost reconfigure branches, which a real
// adapter produces only under a racing resize or a device loss — is driven deterministically here
// instead of being hoped for on a CI GPU leg.
class FakeSwapchain : public ISwapchain
{
public:
    FakeSwapchain(Extent2D size, TextureFormat format, PresentMode mode)
        : size_(size), format_(format), mode_(mode),
          backbuffer_(std::make_unique<FakeTexture>(size, format)),
          view_(backbuffer_->create_view())
    {
    }

    [[nodiscard]] Extent2D size() const override { return size_; }
    [[nodiscard]] TextureFormat format() const override { return format_; }
    [[nodiscard]] PresentMode present_mode() const override { return mode_; }
    [[nodiscard]] bool is_configured() const override { return configured_; }

    AcquiredFrame acquire() override
    {
        ++acquire_count_;
        AcquiredFrame frame;
        if (!configured_)
        {
            frame.status = AcquireStatus::Error;
            return frame;
        }
        if (!scripted_.empty())
        {
            frame.status = scripted_.front();
            scripted_.erase(scripted_.begin());
            if (frame.status != AcquireStatus::Ok)
            {
                return frame; // no view on a non-Ok acquire — exactly the real contract
            }
        }
        else
        {
            frame.status = AcquireStatus::Ok;
        }
        frame.view = view_.get();
        frame.suboptimal = suboptimal_;
        acquired_ = true;
        return frame;
    }

    void present() override
    {
        if (!acquired_)
        {
            return; // presenting without a live acquire is a no-op, never a crash
        }
        acquired_ = false;
        ++present_count_;
    }

    void resize(Extent2D size) override
    {
        if (size.width == 0 || size.height == 0)
        {
            return; // a minimized window keeps its last valid configuration
        }
        size_ = size;
        backbuffer_ = std::make_unique<FakeTexture>(size, format_);
        view_ = backbuffer_->create_view();
        configured_ = true;
        suboptimal_ = false;
        acquired_ = false;
        ++resize_count_;
    }

    void unconfigure() override
    {
        configured_ = false;
        acquired_ = false;
        ++unconfigure_count_;
    }

    // --- test scaffolding ---
    void script_acquire(AcquireStatus status) { scripted_.push_back(status); }
    void set_suboptimal(bool suboptimal) { suboptimal_ = suboptimal; }
    [[nodiscard]] int acquire_count() const { return acquire_count_; }
    [[nodiscard]] int present_count() const { return present_count_; }
    [[nodiscard]] int resize_count() const { return resize_count_; }
    [[nodiscard]] int unconfigure_count() const { return unconfigure_count_; }
    [[nodiscard]] FakeTexture& backbuffer() { return *backbuffer_; }

private:
    Extent2D size_;
    TextureFormat format_;
    PresentMode mode_;
    std::unique_ptr<FakeTexture> backbuffer_;
    std::unique_ptr<ITextureView> view_;
    std::vector<AcquireStatus> scripted_;
    bool configured_ = true;
    bool acquired_ = false;
    bool suboptimal_ = false;
    int acquire_count_ = 0;
    int present_count_ = 0;
    int resize_count_ = 0;
    int unconfigure_count_ = 0;
};

class FakeSurface : public ISurface
{
public:
    explicit FakeSurface(SurfaceCaps caps) : caps_(std::move(caps)) {}

    SurfaceCaps capabilities() override { return caps_; }

    std::unique_ptr<ISwapchain> configure(IDevice& /*device*/, const SurfaceConfig& config) override
    {
        // The real backend refuses an unsupported (format, present mode) pair rather than silently
        // substituting one, so the fake does too — that refusal is what the capability check exists
        // to prevent, and a test must be able to observe it.
        if (!caps_.supported || !caps_.supports_format(config.format) ||
            !caps_.supports_present_mode(config.present_mode) || config.size.width == 0 ||
            config.size.height == 0)
        {
            return nullptr;
        }
        ++configure_count_;
        auto swapchain = std::make_unique<FakeSwapchain>(config.size, config.format,
                                                         config.present_mode);
        last_swapchain_ = swapchain.get();
        return swapchain;
    }

    [[nodiscard]] int configure_count() const { return configure_count_; }

    // The most recently configured swapchain, so a test that does not OWN it — a compositor took
    // the unique_ptr — can still script its acquire statuses and read its counters. A non-owning
    // back-pointer: valid only while the caller that took the unique_ptr is alive, which for a test
    // driving that caller is exactly the window it needs.
    [[nodiscard]] FakeSwapchain* last_swapchain() const { return last_swapchain_; }

private:
    SurfaceCaps caps_;
    FakeSwapchain* last_swapchain_ = nullptr;
    int configure_count_ = 0;
};

// The default capability set a healthy desktop surface reports: BGRA8Unorm + the WebGPU-guaranteed
// Fifo, plus Immediate as the opportunistic extra.
inline SurfaceCaps fake_default_surface_caps()
{
    SurfaceCaps caps;
    caps.supported = true;
    caps.formats = {TextureFormat::BGRA8Unorm, TextureFormat::RGBA8Unorm};
    caps.present_modes = {PresentMode::Fifo, PresentMode::Immediate};
    return caps;
}

// The fake factory. adapter_count controls the probe result; device_creations counts how many
// devices were made — a test asserts probe() creates NONE (R-HEAD-002).
class FakeRhi : public IRhi
{
public:
    explicit FakeRhi(std::size_t adapter_count)
        : adapter_count_(adapter_count), surface_caps_(fake_default_surface_caps())
    {
    }

    [[nodiscard]] RhiTier tier() const override { return RhiTier::T1_WebGPU; }
    [[nodiscard]] const char* backend_name() const override { return "fake"; }

    AdapterProbe probe() override
    {
        AdapterProbe result;
        result.adapter_count = adapter_count_;
        result.has_adapter = adapter_count_ > 0;
        result.primary_name = result.has_adapter ? "fake-adapter" : "";
        return result; // can_present stays false: no surface was asked about
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

    std::unique_ptr<ISurface> create_surface(const NativeWindowDesc& window) override
    {
        if (window.kind == NativeWindowKind::None || window.handle == nullptr)
        {
            return nullptr;
        }
        ++surface_creations_;
        return std::make_unique<FakeSurface>(surface_caps_);
    }

    AdapterProbe probe_surface(ISurface& surface) override
    {
        AdapterProbe result = probe();
        // Still device-free (R-HEAD-002) — presentability is answered from the surface's caps.
        result.can_present = result.has_adapter && surface.capabilities().supported;
        return result;
    }

    [[nodiscard]] int device_creations() const { return device_creations_; }
    [[nodiscard]] int surface_creations() const { return surface_creations_; }
    void set_surface_caps(SurfaceCaps caps) { surface_caps_ = std::move(caps); }

private:
    std::size_t adapter_count_;
    SurfaceCaps surface_caps_;
    int device_creations_ = 0;
    int surface_creations_ = 0;
};

} // namespace rendertest
