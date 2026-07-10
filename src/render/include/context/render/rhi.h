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
#include <optional>
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
// offscreen-readback target format; BGRA8Unorm is the usual swapchain format (windowed path, later);
// Depth32Float is the shadow-map / depth-attachment format (R-REND-004 directional shadow mapping).
enum class TextureFormat
{
    RGBA8Unorm,
    BGRA8Unorm,
    Depth32Float,
};

// WebGPU comparison function — depth testing (RenderPipelineDesc::depth) and comparison samplers
// (shadow-map PCF, SamplerDesc::compare). Undefined = "no comparison" for a regular sampler.
enum class CompareFunction
{
    Undefined,
    Never,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
    Always,
};

enum class FilterMode
{
    Nearest,
    Linear,
};

enum class AddressMode
{
    ClampToEdge,
    Repeat,
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
    // Usage flags (WebGPU semantics): a render-pass attachment (color or depth, by format), a copy
    // source for readback, a copy destination for CPU uploads (IQueue::write_texture), and/or a
    // sampled binding in a shader (bind groups — shadow maps, lightmap inputs).
    bool render_attachment = false;
    bool copy_src = false;
    bool copy_dst = false;
    bool texture_binding = false;
};

struct BufferDesc
{
    std::uint64_t size = 0;
    // Usage flags: a copy destination (texture->buffer readback or IQueue::write_buffer upload),
    // CPU-mappable for read, and/or a uniform binding in a shader.
    bool copy_dst = false;
    bool map_read = false;
    bool uniform = false;
};

struct SamplerDesc
{
    FilterMode min_filter = FilterMode::Nearest;
    FilterMode mag_filter = FilterMode::Nearest;
    AddressMode address_mode = AddressMode::ClampToEdge;
    // != Undefined makes this a COMPARISON sampler (WGSL `sampler_comparison`) — with Linear filters
    // the backend performs the WebGPU-standard 2x2 comparison filter, the PCF hardware primitive the
    // shadow path builds on (R-REND-004).
    CompareFunction compare = CompareFunction::Undefined;
};

// The depth half of a render pipeline (no stencil in the T1 baseline — Depth32Float only).
struct DepthState
{
    TextureFormat format = TextureFormat::Depth32Float;
    bool depth_write = true;
    CompareFunction depth_compare = CompareFunction::Less;
};

struct RenderPipelineDesc
{
    // WGSL is the sole shader language on the web path and the RHI's authoring language (R-REND-005).
    std::string wgsl;
    std::string vertex_entry = "vs_main";
    // An EMPTY fragment_entry makes a depth-only pipeline (no fragment stage, no color target) — the
    // shadow-map depth pass. color_format is ignored then.
    std::string fragment_entry = "fs_main";
    TextureFormat color_format = TextureFormat::RGBA8Unorm;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    // Depth testing/writing against the pass's depth attachment; nullopt = no depth (the 2D paths).
    std::optional<DepthState> depth;
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

// The depth attachment of a render pass (Depth32Float). A pass may be depth-only (no color — the
// shadow-map pass) or color+depth; `clear_depth` follows the WebGPU [0,1] depth range.
struct DepthAttachment
{
    ITextureView* view = nullptr;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    float clear_depth = 1.0f;
};

struct RenderPassDesc
{
    std::vector<ColorAttachment> color;
    std::optional<DepthAttachment> depth;
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

class ISampler
{
public:
    virtual ~ISampler() = default;
};

// An opaque bind-group layout, obtained ONLY by reflecting a pipeline (IRenderPipeline::
// bind_group_layout — WebGPU "auto" layout). The RHI deliberately has no explicit-layout creation
// path: binding numbers are a property of the SHADER the pipeline was built from, so resources are
// always bound against what that shader actually declares. This is what keeps the seam stable under
// the T3d WGSL finding (docs/wgsl-tool-decision.md): when a shader arrives through the cross-compile
// chain, Tint SPLITS combined GLSL samplers into texture+sampler pairs and RENUMBERS bindings — a
// layout reflected from the translated WGSL already carries the post-split numbering, so no caller
// ever hardcodes a pre-split binding map.
class IBindGroupLayout
{
public:
    virtual ~IBindGroupLayout() = default;
};

class IBindGroup
{
public:
    virtual ~IBindGroup() = default;
};

// One resource bound at `binding` — exactly one of buffer / texture / sampler is set (WebGPU
// BindGroupEntry semantics). buffer_size 0 means "to the end of the buffer".
struct BindGroupEntry
{
    std::uint32_t binding = 0;
    IBuffer* buffer = nullptr;
    std::uint64_t buffer_offset = 0;
    std::uint64_t buffer_size = 0;
    ITextureView* texture = nullptr;
    ISampler* sampler = nullptr;
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

    // Reflect the layout of bind group `group` from the pipeline's shader (WebGPU "auto" layout).
    // See IBindGroupLayout for why reflection is the ONLY layout source at this seam.
    [[nodiscard]] virtual std::unique_ptr<IBindGroupLayout>
    bind_group_layout(std::uint32_t group) = 0;
};

// Records the draw commands of one render pass.
class IRenderPassEncoder
{
public:
    virtual ~IRenderPassEncoder() = default;

    virtual void set_pipeline(IRenderPipeline& pipeline) = 0;
    virtual void set_bind_group(std::uint32_t index, IBindGroup& group) = 0;
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

    // CPU->GPU uploads (WebGPU queue-write semantics). write_buffer feeds uniform data (per-frame
    // scene/light/material state extracted from the sim snapshot — R-REND-003: render consumes the
    // copy, never the sim); write_texture uploads texel data (e.g. a lightmap INPUT, R-REND-006).
    virtual void write_buffer(IBuffer& buffer, std::uint64_t offset, const void* data,
                              std::size_t size) = 0;
    virtual void write_texture(ITexture& texture, const void* data, std::size_t size,
                               const TexelCopyBufferLayout& layout, Extent2D extent) = 0;
};

// A live GPU device — created ONLY when rendering is attached (never in a headless build; R-HEAD-002).
class IDevice
{
public:
    virtual ~IDevice() = default;

    [[nodiscard]] virtual IQueue& queue() = 0;

    [[nodiscard]] virtual std::unique_ptr<ITexture> create_texture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<IBuffer> create_buffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<ISampler> create_sampler(const SamplerDesc& desc) = 0;
    [[nodiscard]] virtual std::unique_ptr<IRenderPipeline>
    create_render_pipeline(const RenderPipelineDesc& desc) = 0;
    // Bind resources against a layout reflected from a pipeline (IRenderPipeline::bind_group_layout).
    [[nodiscard]] virtual std::unique_ptr<IBindGroup>
    create_bind_group(IBindGroupLayout& layout, const std::vector<BindGroupEntry>& entries) = 0;
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
