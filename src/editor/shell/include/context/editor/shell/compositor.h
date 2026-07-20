// The per-window compositor (design 03 §4) — the layer stack, damage-driven redraw, the resize
// protocol, and BOTH present paths.
//
// One frame, in order:
//
//   1. acquire the swapchain backbuffer;
//   2. draw the VIEWPORT layers — each viewport panel's render target into its content rect
//      (rect slots today; live content arrives with e11);
//   3. draw the full-window CEF layer, premultiplied, over them. Editor-core keeps viewport content
//      rects transparent (alpha 0) so native content shows through — the "transparent hole"
//      contract, which is what makes CEF chrome (menus, drag ghosts) draw OVER a viewport for free;
//   4. draw the PET_POPUP layer — a SECOND OSR layer confined to the popup rect. Required for
//      production: every dropdown and <select> depends on it, and the spike explicitly skipped it;
//   5. present.
//
// TWO PRESENT PATHS, chosen once at attach:
//
//   * GPU (`attach_gpu`) — the e03 swapchain + composite pass.
//   * CPU (`attach_cpu`, review C-F2) — no usable adapter, so the composed software-OSR buffer goes
//     through an OS 2D primitive (GDI StretchDIBits on Windows). This is what makes "the editor UI
//     never REQUIRES a GPU" mechanized rather than aspirational. The popup is composited into the
//     view buffer on the CPU first, so the fallback is not silently popup-less.
//
// REDRAW IS DAMAGE-DRIVEN, not a frame loop. Engine render rate is decoupled from CEF's paint rate
// (measured in the spike), so an undamaged frame is SKIPPED rather than re-presented — a shell that
// presented unconditionally would burn a GPU queue submit per vsync on a completely static editor.
//
// EVERYTHING HERE IS CEF-FREE AND GPU-BACKEND-FREE: it drives the rhi.h abstraction and the
// browser.h seam, so all of it is unit-tested on the local dev gate and all three CI `build` legs.

#pragma once

#include "context/editor/shell/browser.h"
#include "context/editor/shell/dpi.h"
#include "context/render/present/osr_composite.h"
#include "context/render/present/osr_import.h"
#include "context/render/present/present_blit.h"
#include "context/render/rhi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::shell
{

// Why the next frame is being drawn. Kept as separate flags rather than one bool because "what
// damaged this frame" is the first question when a shell is redrawing more than it should — the
// flags are a diagnostic record, and only any() gates the redraw. (They do NOT drive the resize
// protocol: on_resize reconfigures the swapchain directly and EditorWindow drives WasResized() off
// the resize event, so neither consults damage_.resize.)
struct Damage
{
    bool browser_paint = false;
    bool popup = false;
    // e11's seam: set when a viewport's CONTENT changes without its rect changing. Nothing calls
    // mark_viewport_content() yet — the native viewport consumer lands with e11 — so this is
    // currently always false and `layout` covers a viewport republish.
    bool viewport_content = false;
    bool layout = false;
    bool resize = false;
    bool external = false; // an OS repaint request, a first frame, a device rebuild

    [[nodiscard]] bool any() const
    {
        return browser_paint || popup || viewport_content || layout || resize || external;
    }
    void clear() { *this = Damage{}; }
};

// One viewport panel's slot in this window. `content` is the panel's render-target view; null means
// the slot is published but has no pixels yet — the clear colour shows through rather than the
// previous frame's, which is what keeps a viewport from ghosting while it is being set up.
struct ViewportLayer
{
    std::string id;
    render::Rect2D content_rect; // PHYSICAL pixels, matching the region map
    render::ITextureView* content = nullptr;
    render::Extent2D content_size;
};

enum class PresentPath
{
    none,          // nothing attached yet
    gpu_swapchain, // e03's ISwapchain + composite pass
    cpu_blit,      // C-F2: the OS 2D primitive
};

struct CompositorConfig
{
    render::present::PresentPlatform platform = render::present::current_present_platform();
    render::present::OsrImportOptions import_options;
    // The colour behind every layer. Opaque black: a transparent clear would let the desktop show
    // through the transparent-hole regions of the CEF layer before a viewport has any content.
    render::Color clear{0.0, 0.0, 0.0, 1.0};
};

// The UV rect a scissored fullscreen-triangle draw needs so the source's `visible_rect` lands
// EXACTLY on `dst_rect` inside a `dst_size` target.
//
// The composite pass is a fullscreen triangle whose UV is interpolated across the WHOLE target, so
// confining a draw with a scissor is only half the job: the UV has to be EXTRAPOLATED outward such
// that the interpolation happens to be correct inside the scissor. Without this a popup drawn in a
// corner samples whatever part of its texture that corner's UV happens to land on.
//
// The full-window case is the identity: dst_rect == the whole target yields exactly
// `compute_composite_uv(visible_rect, coded_size)`, which the tests assert so the two cannot drift.
[[nodiscard]] render::present::CompositeUv compute_layer_uv(const render::Rect2D& dst_rect,
                                                            render::Extent2D dst_size,
                                                            const render::Rect2D& visible_rect,
                                                            render::Extent2D coded_size);

// Blend a premultiplied BGRA8 source over a premultiplied BGRA8 destination, in place, at `origin`.
// The CPU present path's popup compositing (step 4 above).
//
// Deliberately NOT `composite_reference_cpu`: that one is e03's GPU ORACLE and writes RGBA8. This is
// the SHIPPING arithmetic for the fallback path, over a BGRA8 destination the blitter can hand
// straight to the OS. Routing through the oracle would mean a swizzle per frame purely to reuse a
// function whose destination format is wrong.
void blend_premultiplied_bgra(std::uint8_t* dst, render::Extent2D dst_size,
                              std::uint32_t dst_bytes_per_row, const std::uint8_t* src,
                              render::Extent2D src_size, std::uint32_t src_bytes_per_row,
                              render::Origin2D origin);

struct CompositorStats
{
    int frames_attempted = 0;
    int frames_presented = 0;
    int frames_skipped_no_damage = 0;
    int reconfigures = 0;
    int view_frames = 0;
    int popup_frames = 0;
    int viewport_draws = 0;
    int popup_draws = 0;
    int acquire_failures = 0;
};

class WindowCompositor final : public IBrowserFrameSink
{
public:
    explicit WindowCompositor(const CompositorConfig& config);

    WindowCompositor(const WindowCompositor&) = delete;
    WindowCompositor& operator=(const WindowCompositor&) = delete;

    // --- attaching a present path ------------------------------------------------------------
    // Configure `surface` against `device` and take the GPU path. false (+ diagnostic()) when the
    // surface cannot be configured — the caller then takes the CPU path rather than failing.
    bool attach_gpu(render::IDevice& device, render::ISurface& surface, render::Extent2D size);
    // Take the CPU present path with an OS blitter. A null blitter is accepted and reported: the
    // shell still runs, it just presents nothing, which is the honest state on a platform whose
    // blitter is e12's.
    void attach_cpu(std::unique_ptr<render::present::IPresentBlitter> blitter,
                    render::Extent2D size);
    void detach();

    [[nodiscard]] PresentPath path() const { return path_; }
    [[nodiscard]] const std::string& diagnostic() const { return diagnostic_; }
    [[nodiscard]] render::Extent2D size() const { return size_; }

    // --- the layer stack -----------------------------------------------------------------------
    void publish_viewports(std::vector<ViewportLayer> layers);
    [[nodiscard]] const std::vector<ViewportLayer>& viewports() const { return viewports_; }

    // --- damage ---------------------------------------------------------------------------------
    void mark_external_damage() { damage_.external = true; }
    void mark_viewport_content() { damage_.viewport_content = true; }
    [[nodiscard]] const Damage& damage() const { return damage_; }

    // --- IBrowserFrameSink ---------------------------------------------------------------------
    void on_browser_frame(const BrowserFrame& frame) override;
    void on_popup_state(bool visible, const render::Rect2D& rect) override;

    // --- window lifecycle -----------------------------------------------------------------------
    // A new PHYSICAL client size. Reconfigures the swapchain and damages the frame. A zero extent is
    // ignored (a minimized window reports one every frame); an unchanged size is a no-op so a
    // spurious WM_SIZE does not force a reconfigure.
    void on_resize(render::Extent2D physical_size);

    // Draw + present one frame if anything is damaged. Returns true when it presented.
    bool render_frame();

    [[nodiscard]] const CompositorStats& stats() const { return stats_; }
    [[nodiscard]] bool popup_visible() const { return popup_visible_; }
    [[nodiscard]] const render::Rect2D& popup_rect() const { return popup_rect_; }
    [[nodiscard]] const render::present::OsrTextureImporter& view_importer() const
    {
        return view_importer_;
    }
    // The composed CPU surface of the last cpu_blit frame — the honest present target when the
    // blitter is e03's MemoryBlitter, and what the Session-0-safe smoke asserts pixels from.
    [[nodiscard]] const std::vector<std::uint8_t>& cpu_surface() const { return cpu_surface_; }
    [[nodiscard]] render::present::IPresentBlitter* blitter() const { return blitter_.get(); }

private:
    struct CpuFrame
    {
        std::vector<std::uint8_t> pixels;
        std::uint32_t bytes_per_row = 0;
        render::Extent2D coded_size;
        render::Rect2D visible_rect;
        bool valid = false;
    };

    bool ensure_gpu_resources();
    bool render_gpu_frame();
    bool render_cpu_frame();
    // Draw one composite layer into the current pass: scissor to `dst_rect`, bind `view` with the
    // extrapolated UV, draw the fullscreen triangle. Returns false when nothing was drawn — the rect
    // clipped to nothing, or a resource could not be created — so the caller does not advance the
    // slot counter past a slot that was never populated.
    [[nodiscard]] bool draw_layer(render::IRenderPassEncoder& pass, const render::Rect2D& dst_rect,
                                  render::ITextureView& view, const render::Rect2D& visible_rect,
                                  render::Extent2D coded_size, std::size_t slot);
    void reconfigure();
    // Copy a producer frame into CPU storage (the fallback path presents it later, after the
    // callback's pointer is gone).
    static void capture_cpu_frame(CpuFrame& out, const render::present::OsrFrame& frame);

    CompositorConfig config_;
    PresentPath path_ = PresentPath::none;
    std::string diagnostic_;

    render::IDevice* device_ = nullptr;
    render::ISurface* surface_ = nullptr;
    std::unique_ptr<render::ISwapchain> swapchain_;
    std::unique_ptr<render::present::IPresentBlitter> blitter_;
    render::Extent2D size_{};

    // Composite resources, built once against the swapchain's format and reused every frame. A
    // pipeline rebuilt per frame is a driver-side recompile per frame.
    std::unique_ptr<render::IRenderPipeline> pipeline_;
    std::unique_ptr<render::IBindGroupLayout> bind_layout_;
    std::unique_ptr<render::ISampler> sampler_;
    // One uniform buffer + bind group per LAYER SLOT drawn in a frame. They cannot be shared: a
    // single buffer rewritten between draws in the same pass would have every draw read whichever
    // write landed last, so all layers would sample the final layer's UV rect.
    std::vector<std::unique_ptr<render::IBuffer>> layer_uniforms_;
    std::vector<std::unique_ptr<render::IBindGroup>> layer_bind_groups_;

    render::present::OsrTextureImporter view_importer_;
    render::present::OsrTextureImporter popup_importer_;
    render::Rect2D view_visible_rect_{};
    render::Extent2D view_coded_size_{};
    render::Rect2D popup_visible_rect_{};
    render::Extent2D popup_coded_size_{};
    bool have_view_frame_ = false;
    bool have_popup_frame_ = false;

    CpuFrame cpu_view_;
    CpuFrame cpu_popup_;
    std::vector<std::uint8_t> cpu_surface_;

    std::vector<ViewportLayer> viewports_;
    bool popup_visible_ = false;
    render::Rect2D popup_rect_{};

    Damage damage_;
    CompositorStats stats_;
};

} // namespace context::editor::shell
