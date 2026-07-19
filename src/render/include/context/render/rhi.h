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
//
// M9 e03 added the WINDOWED half — ISurface/ISwapchain, the external-texture (OSR) import seam, and
// colour blending — so the object model now covers presentation as well as offscreen rendering. It
// stays dependency-free: native window handles arrive as an opaque void* plus a kind tag, so no
// <windows.h>/X11/Cocoa type reaches this header and a headless TU can still include it. The policy
// and passes built on these seams live in context/render/present/; see docs/present-path.md.

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

// The top-left texel of a texture sub-rect (WebGPU TexelCopyTextureInfo::origin). Used by the
// sub-rect upload path (IQueue::write_texture_region) — the OSR dirty-rect blit.
struct Origin2D
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

// An axis-aligned texel rect (origin + extent) — the OSR dirty rect and the CEF `visible_rect`.
struct Rect2D
{
    Origin2D origin;
    Extent2D size;
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

// Colour-blend factors (the WebGPU subset the T1 paths need). The premultiplied-alpha composite the
// window compositor draws the OSR layer with is exactly One / OneMinusSrcAlpha (03 §4).
enum class BlendFactor
{
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
};

enum class BlendOperation
{
    Add,
};

struct BlendComponent
{
    BlendFactor src = BlendFactor::One;
    BlendFactor dst = BlendFactor::Zero;
    BlendOperation op = BlendOperation::Add;
};

// Per-target blend state. ABSENT (RenderPipelineDesc::blend == nullopt) means "replace" — the
// WebGPU default and what every pre-existing pipeline in this repo relies on, so adding this field
// changes no existing behavior.
struct BlendState
{
    BlendComponent color;
    BlendComponent alpha;
};

// The premultiplied-alpha "source-over" state (blend ONE / INV_SRC_ALPHA on both channels) — the
// one the CEF OSR layer is composited with (03 §4, renderer_d3d11.cpp:152-161 reference).
[[nodiscard]] inline BlendState premultiplied_alpha_blend()
{
    BlendState state;
    state.color.src = BlendFactor::One;
    state.color.dst = BlendFactor::OneMinusSrcAlpha;
    state.alpha.src = BlendFactor::One;
    state.alpha.dst = BlendFactor::OneMinusSrcAlpha;
    return state;
}

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
    // Colour-target blending; nullopt = replace (the WebGPU default every earlier pipeline uses).
    std::optional<BlendState> blend;
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
    // copy, never the sim).
    virtual void write_buffer(IBuffer& buffer, std::uint64_t offset, const void* data,
                              std::size_t size) = 0;

    // Upload texel data into the sub-rect at `origin` (WebGPU TexelCopyTextureInfo::origin). This is
    // the GENERAL texture-upload form and the one the OSR dirty-rect path drives: a CEF `OnPaint`
    // reports N damaged rects, and re-uploading only those is what keeps the software OSR path
    // affordable (03 §3). `data` points at the sub-rect's OWN first texel; `layout.bytes_per_row` is
    // that sub-image's stride, which for a dirty rect carved out of a larger frame is the SOURCE
    // frame's stride, not the rect's width.
    virtual void write_texture_region(ITexture& texture, Origin2D origin, const void* data,
                                      std::size_t size, const TexelCopyBufferLayout& layout,
                                      Extent2D extent) = 0;

    // Whole-texture upload (origin 0,0) — e.g. a lightmap INPUT (R-REND-006). A non-virtual
    // convenience over write_texture_region so a backend implements the sub-rect form ONCE.
    void write_texture(ITexture& texture, const void* data, std::size_t size,
                       const TexelCopyBufferLayout& layout, Extent2D extent)
    {
        write_texture_region(texture, Origin2D{}, data, size, layout, extent);
    }
};

// ---------------------------------------------------------------------------------------------
// External-texture import — the OSR (off-screen rendering) interop seam (03 §3). A browser/compositor
// produces each UI frame OUTSIDE this device; import_external_texture adopts that frame as an ITexture
// the composite pass can sample.
//
// Two tiers, and which one a platform gets is a POLICY decision made one layer up
// (context/render/present/osr_import.h) — never silently here:
//   * ACCELERATED (zero-copy): the producer's GPU allocation is adopted directly. Available on macOS
//     via wgpu-native's STOCK native Metal accessors (raw IOSurface -> MTLTexture -> blit).
//   * CPU UPLOAD (always works): the producer's BGRA8 pixels are uploaded with write_texture_region,
//     dirty-rect by dirty-rect. This is the SHIPPING path on Windows and the Linux default.
//
// Windows note (owner ruling 2026-07-19): DXGI shared-handle import is NOT reachable — stock
// wgpu-native's C API exposes no external-texture import, and carrying a patched fork was rejected as
// an unbounded maintenance cost. The upstream ask is gfx-rs/wgpu-native#621; when the C API gains
// import, D3D12SharedHandle becomes implementable with NO change to this seam. Until then a backend
// asked for it FAILS CLOSED (null texture + a diagnostic), never a silent degrade.
// ---------------------------------------------------------------------------------------------
enum class ExternalTextureSource
{
    CpuBgra,           // CPU BGRA8 pixels uploaded per dirty rect — always available
    D3D12SharedHandle, // Windows DXGI NT shared handle (zero-copy) — see wgpu-native#621
    IOSurface,         // macOS raw IOSurface (zero-copy) via the stock native Metal accessors
    DmaBuf,            // Linux dmabuf (zero-copy) — behind the accel gate, ships OFF
};

struct ExternalTextureDesc
{
    ExternalTextureSource source = ExternalTextureSource::CpuBgra;
    // The producer's FULL allocation size. CEF's `coded_size` — usually >= the visible rect, which is
    // why the composite pass scales UVs by visible_rect/coded_size rather than sampling [0,1].
    Extent2D coded_size;
    // The platform handle for an accelerated source (HANDLE / IOSurfaceRef / dmabuf fd). Ignored — and
    // expected null — for CpuBgra, whose pixels arrive later through IQueue::write_texture_region.
    void* handle = nullptr;
};

// The RESULT of an import: what the backend actually did, so a caller never has to guess whether it
// got the zero-copy path. `texture == nullptr` means the import FAILED and `diagnostic` says why.
struct ExternalTexture
{
    std::unique_ptr<ITexture> texture;
    ExternalTextureSource source_used = ExternalTextureSource::CpuBgra;
    bool accelerated = false;
    std::string diagnostic;
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

    // Adopt an externally-produced frame as a sampleable texture (the OSR interop seam above).
    // FAILS CLOSED — a source this backend cannot honour returns a null texture plus a diagnostic,
    // never a quietly-substituted slower path; choosing the path is the caller's job.
    [[nodiscard]] virtual ExternalTexture
    import_external_texture(const ExternalTextureDesc& desc) = 0;

    // Refresh an ACCELERATED import from the producer's newest frame, returning an empty string on
    // success or the reason it failed. Needed because not every accelerated path is a one-shot
    // adoption: macOS delivers a NEW IOSurface per paint, which is blitted GPU-side into the
    // imported texture, so an import that ran only once would freeze the UI on its first frame.
    // The CpuBgra path does not use this — it refreshes through write_texture_region.
    [[nodiscard]] virtual std::string
    refresh_external_texture(ITexture& texture, const ExternalTextureDesc& desc) = 0;
};

// ---------------------------------------------------------------------------------------------
// Surface / swapchain — the WINDOWED present path (R-HEAD-004; design 03 §2). Offscreen rendering
// needs NO surface, and the headless invariant is that nothing in the daemon/runtime reaches this
// path at all: only the Shell creates a surface, so a headless build links no presentation code
// (asserted structurally — cmake/ContextPresentIsolation.cmake).
//
// The object model is WebGPU's: the instance wraps a native window as an ISurface; the surface is
// CONFIGURED against a device to produce an ISwapchain; each frame acquires the current backbuffer
// view, renders into it, and presents. A resize reconfigures in place.
// ---------------------------------------------------------------------------------------------

// Frame pacing (WebGPU present modes). Fifo is the DEFAULT and the only mode WebGPU guarantees to
// exist on every surface — the others are opportunistic and must be capability-checked first.
enum class PresentMode
{
    Fifo,
    FifoRelaxed,
    Immediate,
    Mailbox,
};

// Which flavour of native window handle a NativeWindowDesc carries. Deliberately an OPAQUE void*
// model: rhi.h stays free of <windows.h> / X11 / Cocoa so it remains includable from headless code.
enum class NativeWindowKind
{
    None,
    Win32Hwnd,      // handle = HWND, display = HINSTANCE
    XlibWindow,     // handle = Window (an XID widened to a pointer), display = Display*
    WaylandSurface, // handle = wl_surface*, display = wl_display*
    MetalLayer,     // handle = CAMetalLayer* (the layer backing the NSView)
};

struct NativeWindowDesc
{
    NativeWindowKind kind = NativeWindowKind::None;
    void* handle = nullptr;
    void* display = nullptr;
};

struct SurfaceConfig
{
    Extent2D size;
    // BGRA8Unorm is the usual swapchain format (and what the spike proved); viewport render targets
    // stay RGBA8Unorm. Capability-check before configuring — see SurfaceCaps.
    TextureFormat format = TextureFormat::BGRA8Unorm;
    PresentMode present_mode = PresentMode::Fifo;
};

// What a (surface, adapter) pair actually supports — the input to the capability check the spike
// performs before configuring (spikes/webgpu/main.cpp:546-560).
struct SurfaceCaps
{
    // False = this adapter cannot present to this surface at all (drives the CPU present fallback).
    bool supported = false;
    std::vector<TextureFormat> formats;
    std::vector<PresentMode> present_modes;

    [[nodiscard]] bool supports_format(TextureFormat format) const
    {
        for (TextureFormat f : formats)
        {
            if (f == format)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool supports_present_mode(PresentMode mode) const
    {
        for (PresentMode m : present_modes)
        {
            if (m == mode)
            {
                return true;
            }
        }
        return false;
    }
};

// The outcome of acquiring a backbuffer. Only Ok yields a usable view. Outdated/Lost are NORMAL,
// expected events (a resize raced the frame; the device went away) that the compositor handles by
// reconfiguring — they are not errors and must not be reported as such.
enum class AcquireStatus
{
    Ok,
    Outdated,
    Lost,
    Timeout,
    Error,
};

struct AcquiredFrame
{
    ITextureView* view = nullptr;
    AcquireStatus status = AcquireStatus::Error;
    // True when the frame is presentable but no longer optimally configured (a pending resize) —
    // present it, then reconfigure. Distinct from Outdated, which yields NO frame at all.
    bool suboptimal = false;
};

// A configured presentation chain over one surface. Not thread-safe: one compositor owns it.
class ISwapchain
{
public:
    virtual ~ISwapchain() = default;

    [[nodiscard]] virtual Extent2D size() const = 0;
    [[nodiscard]] virtual TextureFormat format() const = 0;
    [[nodiscard]] virtual PresentMode present_mode() const = 0;
    [[nodiscard]] virtual bool is_configured() const = 0;

    // Acquire the current backbuffer view. The view is owned by the swapchain and is valid ONLY
    // until the matching present() — never retain it across frames.
    [[nodiscard]] virtual AcquiredFrame acquire() = 0;

    // Show the acquired frame. A no-op when the last acquire() did not return Ok.
    virtual void present() = 0;

    // Reconfigure for a new window size (the resize protocol, 03 §4). A zero extent is ignored —
    // a minimized window keeps its last valid configuration rather than tearing the chain down.
    virtual void resize(Extent2D size) = 0;

    // Release the presentation chain (teardown, or before the window is destroyed). Idempotent.
    virtual void unconfigure() = 0;
};

// A presentable native window. Created by IRhi::create_surface; configured against a device to get
// the swapchain.
class ISurface
{
public:
    virtual ~ISurface() = default;

    // What this surface supports on the backend's default adapter. `supported == false` is a clean
    // report (the CPU present fallback takes over), not an error.
    [[nodiscard]] virtual SurfaceCaps capabilities() = 0;

    // Configure the surface against `device` and return its swapchain, or nullptr when `config` is
    // unsupported (check capabilities() first) or the surface is unusable.
    [[nodiscard]] virtual std::unique_ptr<ISwapchain> configure(IDevice& device,
                                                                const SurfaceConfig& config) = 0;
};

// The result of enumerating adapters WITHOUT creating a device — the R-HEAD-002 headless probe.
// has_adapter == false is a clean, successful report (a GPU-less box), never an error.
struct AdapterProbe
{
    std::size_t adapter_count = 0;
    bool has_adapter = false;
    std::string primary_name;
    // The editor's GPU gate (03 §2): can the probed adapter actually PRESENT to a given surface?
    // Only IRhi::probe_surface() sets this — the surface-less probe() always reports false, because
    // "no surface was asked about" is not "cannot present". A false here with has_adapter true is
    // the honest signal that drives the CPU present fallback (C-F2) rather than a hard failure.
    bool can_present = false;
};

// The backend factory. probe() creates no device (R-HEAD-002); create_device() returns nullptr when
// no adapter is available, so the caller can stay headless without a hard failure. create_surface()
// is the entry to the WINDOWED present path (see ISurface below) — it is the Shell's alone; nothing
// in the daemon/runtime ever calls it (the headless invariant, structurally asserted by
// cmake/ContextPresentIsolation.cmake).
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

    // Wrap a native window as a presentable surface. Returns nullptr when the descriptor names no
    // window, or names a window kind this backend/platform cannot wrap — a clean, reportable
    // absence (the caller degrades to the CPU present path), never a crash.
    [[nodiscard]] virtual std::unique_ptr<ISurface>
    create_surface(const NativeWindowDesc& window) = 0;

    // The surface-aware probe (03 §2): enumerate adapters AND report whether one can present to
    // `surface`, still WITHOUT creating a device. This is the editor's GPU gate.
    [[nodiscard]] virtual AdapterProbe probe_surface(ISurface& surface) = 0;
};

} // namespace context::render
