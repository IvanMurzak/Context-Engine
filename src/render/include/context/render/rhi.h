// Render Hardware Interface (RHI) — the tiered, WebGPU-semantics abstraction the renderer draws
// through (R-REND-001/002, L-11/L-56). T1 (WebGPU baseline, all platforms incl. web) is the floor;
// T2 (native Vulkan/DX12/Metal advanced features) is a capability tier layered on the SAME object
// model. This header is deliberately dependency-free (no wgpu/webgpu types leak here) so it is
// includable from headless code that never links a GPU backend — the R-HEAD-002 detach seam.
//
// The object model mirrors WebGPU: an IRhi factory probes adapters and creates an IDevice; the
// device owns an IQueue and creates buffers / textures / render pipelines / command encoders; a
// command encoder records a render pass (clear + draw) and a texture->buffer copy; the queue submits
// it; a mappable buffer is read back on the CPU. The concrete T1 native backend (wgpu-native) lives
// in context/render/wgpu/; a header-only fake backend in the tests drives this exact surface with no
// GPU, which is what lets the abstraction carry real local test coverage.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace context::render
{

// Capability tier (L-11/L-56). T1 WebGPU is the floor and the only tier this foundation implements
// natively; T2 is the seam for native-advanced features layered on the same interface later.
enum class RhiTier
{
    T1_WebGPU,
    T2_Native,
};

// A minimal WebGPU-shaped texture-format set — extended as the renderer grows. RGBA8Unorm is the
// offscreen-readback target format; BGRA8Unorm is the usual swapchain format (windowed path, later).
enum class TextureFormat
{
    RGBA8Unorm,
    BGRA8Unorm,
};

enum class LoadOp
{
    Clear,
    Load,
};

enum class StoreOp
{
    Store,
    Discard,
};

enum class PrimitiveTopology
{
    TriangleList,
};

struct Color
{
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;
};

struct Extent2D
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct TextureDesc
{
    Extent2D size;
    TextureFormat format = TextureFormat::RGBA8Unorm;
    // Usage flags (WebGPU semantics): a render-pass color target and/or a copy source for readback.
    bool render_attachment = false;
    bool copy_src = false;
};

struct BufferDesc
{
    std::uint64_t size = 0;
    // Usage flags: a copy destination (e.g. texture->buffer) and/or CPU-mappable for read.
    bool copy_dst = false;
    bool map_read = false;
};

struct RenderPipelineDesc
{
    // WGSL is the sole shader language on the web path and the RHI's authoring language (R-REND-005).
    std::string wgsl;
    std::string vertex_entry = "vs_main";
    std::string fragment_entry = "fs_main";
    TextureFormat color_format = TextureFormat::RGBA8Unorm;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
};

class ITextureView;
class IRenderPipeline;

struct ColorAttachment
{
    ITextureView* view = nullptr;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    Color clear;
};

struct RenderPassDesc
{
    std::vector<ColorAttachment> color;
};

// Row layout for a texture->buffer copy (WebGPU's TexelCopyBufferLayout). bytes_per_row must satisfy
// the backend's 256-byte alignment; the caller computes it.
struct TexelCopyBufferLayout
{
    std::uint32_t bytes_per_row = 0;
    std::uint32_t rows_per_image = 0;
};

// A CPU-mappable GPU buffer.
class IBuffer
{
public:
    virtual ~IBuffer() = default;

    [[nodiscard]] virtual std::uint64_t size() const = 0;

    // Map the whole buffer for reading and return a pointer to its bytes (blocking — the backend
    // pumps device events internally until the map resolves). Returns nullptr on failure. Pair with
    // unmap(). Only valid for a buffer created with map_read.
    [[nodiscard]] virtual const void* map_read() = 0;
    virtual void unmap() = 0;
};

class ITextureView
{
public:
    virtual ~ITextureView() = default;
};

class ITexture
{
public:
    virtual ~ITexture() = default;

    [[nodiscard]] virtual std::unique_ptr<ITextureView> create_view() = 0;
};

class IRenderPipeline
{
public:
    virtual ~IRenderPipeline() = default;
};

// Records the draw commands of one render pass.
class IRenderPassEncoder
{
public:
    virtual ~IRenderPassEncoder() = default;

    virtual void set_pipeline(IRenderPipeline& pipeline) = 0;
    virtual void draw(std::uint32_t vertex_count, std::uint32_t instance_count) = 0;
    virtual void end() = 0;
};

// A finished, submittable command buffer.
class ICommandBuffer
{
public:
    virtual ~ICommandBuffer() = default;
};

// Records GPU commands: a render pass, then copies, then finish() into a command buffer.
class ICommandEncoder
{
public:
    virtual ~ICommandEncoder() = default;

    [[nodiscard]] virtual std::unique_ptr<IRenderPassEncoder>
    begin_render_pass(const RenderPassDesc& desc) = 0;

    virtual void copy_texture_to_buffer(ITexture& src, IBuffer& dst,
                                        const TexelCopyBufferLayout& layout, Extent2D extent) = 0;

    [[nodiscard]] virtual std::unique_ptr<ICommandBuffer> finish() = 0;
};

class IQueue
{
public:
    virtual ~IQueue() = default;

    virtual void submit(ICommandBuffer& commands) = 0;
};

// A live GPU device — created ONLY when rendering is attached (never in a headless build; R-HEAD-002).
class IDevice
{
public:
    virtual ~IDevice() = default;

    [[nodiscard]] virtual IQueue& queue() = 0;

    [[nodiscard]] virtual std::unique_ptr<ITexture> create_texture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<IBuffer> create_buffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<IRenderPipeline>
    create_render_pipeline(const RenderPipelineDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<ICommandEncoder> create_command_encoder() = 0;
};

// The result of enumerating adapters WITHOUT creating a device — the R-HEAD-002 headless probe.
// has_adapter == false is a clean, successful report (a GPU-less box), never an error.
struct AdapterProbe
{
    std::size_t adapter_count = 0;
    bool has_adapter = false;
    std::string primary_name;
};

// The backend factory. probe() creates no device (R-HEAD-002); create_device() returns nullptr when
// no adapter is available, so the caller can stay headless without a hard failure. Surface/swapchain
// (the windowed present path) are a later wave — see ISurface below.
class IRhi
{
public:
    virtual ~IRhi() = default;

    [[nodiscard]] virtual RhiTier tier() const = 0;
    [[nodiscard]] virtual const char* backend_name() const = 0;

    // Enumerate adapters and report their presence/absence WITHOUT creating a GPU device.
    [[nodiscard]] virtual AdapterProbe probe() = 0;

    // Create a device on the default adapter, or nullptr when no adapter exists.
    [[nodiscard]] virtual std::unique_ptr<IDevice> create_device() = 0;
};

// ---------------------------------------------------------------------------------------------
// Surface / swapchain — the WINDOWED present path (R-HEAD-004 SHOULD; the T1 object model names it
// for completeness). Offscreen rendering (this foundation's proof) needs NO surface, so these are
// declared here as the seam and implemented when the windowed/web-canvas present path lands in a
// later wave. Keeping them in the object model now means the renderer's present path is an additive
// change against a stable interface rather than a reshaping of it.
// ---------------------------------------------------------------------------------------------
class ISurface
{
public:
    virtual ~ISurface() = default;
};

class ISwapchain
{
public:
    virtual ~ISwapchain() = default;
};

} // namespace context::render
