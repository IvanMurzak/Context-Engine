// The per-window compositor — see compositor.h for the layer order, the two present paths, and why
// redraw is damage-driven.

#include "context/editor/shell/compositor.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace context::editor::shell
{
namespace
{

using render::present::CompositeUv;

// Clip a rect to a target of `bounds`, returning an empty extent when it falls fully outside.
[[nodiscard]] render::Rect2D clip_to(const render::Rect2D& rect, render::Extent2D bounds)
{
    render::Rect2D clipped;
    if (render::is_empty(rect.size) || render::is_empty(bounds) || rect.origin.x >= bounds.width ||
        rect.origin.y >= bounds.height)
    {
        return clipped;
    }
    clipped.origin = rect.origin;
    clipped.size.width = std::min(rect.size.width, bounds.width - rect.origin.x);
    clipped.size.height = std::min(rect.size.height, bounds.height - rect.origin.y);
    return clipped;
}

// Premultiplied "source over destination" on one 8-bit channel: dst = src + dst * (1 - a).
// Integer arithmetic with round-to-nearest (+127)/255 rather than a shift, so a long chain of
// composites does not drift darker one least-significant bit at a time.
[[nodiscard]] std::uint8_t over_channel(std::uint8_t src, std::uint8_t dst, std::uint8_t alpha)
{
    const std::uint32_t inv = 255u - alpha;
    const std::uint32_t scaled = (static_cast<std::uint32_t>(dst) * inv + 127u) / 255u;
    const std::uint32_t sum = static_cast<std::uint32_t>(src) + scaled;
    return static_cast<std::uint8_t>(sum > 255u ? 255u : sum);
}

} // namespace

CompositeUv compute_layer_uv(const render::Rect2D& dst_rect, render::Extent2D dst_size,
                             const render::Rect2D& visible_rect, render::Extent2D coded_size)
{
    // The sub-rect this layer wants to SHOW, in normalized source coordinates. Reusing e03's
    // function is what keeps the degenerate cases (empty coded size, unreported visible rect, an
    // origin a concurrent resize left outside the allocation) answered in exactly one place.
    const CompositeUv want = render::present::compute_composite_uv(visible_rect, coded_size);

    if (render::is_empty(dst_size) || render::is_empty(dst_rect.size))
    {
        return want;
    }

    // Extrapolate so that the fullscreen triangle's linear UV interpolation is correct INSIDE
    // dst_rect: solve for the u at screen x = 0 and x = dst_size.width given u(dst_x0) = want.u0
    // and u(dst_x1) = want.u1. See the header for why a scissor alone is not enough.
    const float dst_x0 = static_cast<float>(dst_rect.origin.x);
    const float dst_y0 = static_cast<float>(dst_rect.origin.y);
    const float dst_w = static_cast<float>(dst_rect.size.width);
    const float dst_h = static_cast<float>(dst_rect.size.height);
    const float full_w = static_cast<float>(dst_size.width);
    const float full_h = static_cast<float>(dst_size.height);

    const float du = (want.u1 - want.u0) / dst_w; // source u per destination pixel
    const float dv = (want.v1 - want.v0) / dst_h;

    CompositeUv uv;
    uv.u0 = want.u0 - du * dst_x0;
    uv.v0 = want.v0 - dv * dst_y0;
    uv.u1 = uv.u0 + du * full_w;
    uv.v1 = uv.v0 + dv * full_h;
    return uv;
}

void blend_premultiplied_bgra(std::uint8_t* dst, render::Extent2D dst_size,
                              std::uint32_t dst_bytes_per_row, const std::uint8_t* src,
                              render::Extent2D src_size, std::uint32_t src_bytes_per_row,
                              render::Origin2D origin)
{
    if (dst == nullptr || src == nullptr || render::is_empty(dst_size) ||
        render::is_empty(src_size) || origin.x >= dst_size.width || origin.y >= dst_size.height)
    {
        return;
    }
    const std::uint32_t width = std::min(src_size.width, dst_size.width - origin.x);
    const std::uint32_t height = std::min(src_size.height, dst_size.height - origin.y);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        const std::uint8_t* src_row = src + static_cast<std::size_t>(y) * src_bytes_per_row;
        std::uint8_t* dst_row = dst + static_cast<std::size_t>(y + origin.y) * dst_bytes_per_row +
                                static_cast<std::size_t>(origin.x) * 4u;
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint8_t* s = src_row + static_cast<std::size_t>(x) * 4u;
            std::uint8_t* d = dst_row + static_cast<std::size_t>(x) * 4u;
            const std::uint8_t alpha = s[3];
            d[0] = over_channel(s[0], d[0], alpha);
            d[1] = over_channel(s[1], d[1], alpha);
            d[2] = over_channel(s[2], d[2], alpha);
            d[3] = over_channel(s[3], d[3], alpha);
        }
    }
}

// ------------------------------------------------------------------------------ WindowCompositor

WindowCompositor::WindowCompositor(const CompositorConfig& config)
    : config_(config), view_importer_(config.platform, config.import_options),
      popup_importer_(config.platform, config.import_options)
{
}

bool WindowCompositor::attach_gpu(render::IDevice& device, render::ISurface& surface,
                                  render::Extent2D size)
{
    device_ = &device;
    surface_ = &surface;
    size_ = size;

    const render::SurfaceCaps caps = surface.capabilities();
    if (!caps.supported)
    {
        path_ = PresentPath::none;
        diagnostic_ = "the surface reports no presentable adapter; take the CPU present path";
        return false;
    }

    render::SurfaceConfig config;
    config.size = size;
    config.format = render::TextureFormat::BGRA8Unorm;
    config.present_mode = render::PresentMode::Fifo;
    // Capability-check before configuring: e03's contract is that configure() REFUSES an unsupported
    // pair rather than substituting one, so an unchecked configure is a null swapchain, not a
    // graceful downgrade.
    if (!caps.supports_format(config.format) || !caps.supports_present_mode(config.present_mode))
    {
        path_ = PresentPath::none;
        diagnostic_ = "the surface supports neither BGRA8Unorm nor Fifo; take the CPU present path";
        return false;
    }

    swapchain_ = surface.configure(device, config);
    if (swapchain_ == nullptr)
    {
        path_ = PresentPath::none;
        diagnostic_ = "the surface refused the BGRA8Unorm/Fifo configuration";
        return false;
    }
    path_ = PresentPath::gpu_swapchain;
    diagnostic_.clear();
    // A freshly attached window has never drawn; without this the first frame waits for a paint that
    // may not come until the user moves the mouse.
    damage_.external = true;
    return ensure_gpu_resources();
}

void WindowCompositor::attach_cpu(std::unique_ptr<render::present::IPresentBlitter> blitter,
                                  render::Extent2D size)
{
    blitter_ = std::move(blitter);
    size_ = size;
    path_ = PresentPath::cpu_blit;
    diagnostic_ = blitter_ == nullptr
                      ? "the CPU present path has no blitter on this platform (X11 SHM / "
                        "CALayer.contents land with e12); the shell runs but presents nothing"
                      : std::string{};
    damage_.external = true;
}

void WindowCompositor::detach()
{
    if (swapchain_ != nullptr)
    {
        swapchain_->unconfigure();
    }
    // The composite resources are owned by the device being detached from; dropping them here rather
    // than at destruction is what keeps a re-attach (a device rebuild after a loss) from binding
    // resources that belong to the dead device.
    layer_bind_groups_.clear();
    layer_uniforms_.clear();
    pipeline_.reset();
    bind_layout_.reset();
    sampler_.reset();
    swapchain_.reset();
    blitter_.reset();
    device_ = nullptr;
    surface_ = nullptr;
    path_ = PresentPath::none;
}

bool WindowCompositor::ensure_gpu_resources()
{
    if (device_ == nullptr || swapchain_ == nullptr)
    {
        return false;
    }
    if (pipeline_ != nullptr)
    {
        return true;
    }
    pipeline_ = device_->create_render_pipeline(
        render::present::make_composite_pipeline_desc(swapchain_->format()));
    if (pipeline_ == nullptr)
    {
        diagnostic_ = "the composite pipeline could not be created";
        return false;
    }
    bind_layout_ = pipeline_->bind_group_layout(0);
    if (bind_layout_ == nullptr)
    {
        diagnostic_ = "the composite pipeline exposed no bind-group layout";
        return false;
    }
    // Nearest: a layer is composited at its native scale, so filtering would only blur text.
    render::SamplerDesc sampler_desc;
    sampler_ = device_->create_sampler(sampler_desc);
    return sampler_ != nullptr;
}

void WindowCompositor::capture_cpu_frame(CpuFrame& out, const render::present::OsrFrame& frame)
{
    out.valid = false;
    if (frame.pixels == nullptr || frame.byte_size == 0 || render::is_empty(frame.coded_size))
    {
        return;
    }
    // Same bounds rule the import driver and the blitters share, so a truncated producer frame is
    // refused here too rather than read out of bounds one layer later.
    if (!render::present::rows_fit(frame.coded_size, frame.bytes_per_row, frame.byte_size))
    {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(frame.pixels);
    out.pixels.assign(bytes, bytes + frame.byte_size);
    out.bytes_per_row = frame.bytes_per_row;
    out.coded_size = frame.coded_size;
    out.visible_rect = frame.visible_rect;
    out.valid = true;
}

void WindowCompositor::on_browser_frame(const BrowserFrame& frame)
{
    if (frame.layer == BrowserLayer::popup)
    {
        ++stats_.popup_frames;
        popup_visible_rect_ = frame.frame.visible_rect;
        popup_coded_size_ = frame.frame.coded_size;
        if (path_ == PresentPath::gpu_swapchain && device_ != nullptr)
        {
            have_popup_frame_ = popup_importer_.update(*device_, frame.frame);
        }
        else
        {
            capture_cpu_frame(cpu_popup_, frame.frame);
            have_popup_frame_ = cpu_popup_.valid;
        }
        damage_.popup = true;
        return;
    }

    ++stats_.view_frames;
    view_visible_rect_ = frame.frame.visible_rect;
    view_coded_size_ = frame.frame.coded_size;
    if (path_ == PresentPath::gpu_swapchain && device_ != nullptr)
    {
        have_view_frame_ = view_importer_.update(*device_, frame.frame);
    }
    else
    {
        capture_cpu_frame(cpu_view_, frame.frame);
        have_view_frame_ = cpu_view_.valid;
    }
    damage_.browser_paint = true;
}

void WindowCompositor::on_popup_state(bool visible, const render::Rect2D& rect)
{
    popup_visible_ = visible;
    popup_rect_ = rect;
    if (!visible)
    {
        // Drop the layer, do not merely stop drawing it — see the seam note in browser.h: CEF reuses
        // the popup texture for the next dropdown, so a retained layer composites stale pixels for
        // the frame between the hide and the next paint.
        have_popup_frame_ = false;
        cpu_popup_ = CpuFrame{};
    }
    damage_.popup = true;
}

void WindowCompositor::publish_viewports(std::vector<ViewportLayer> layers)
{
    viewports_ = std::move(layers);
    damage_.layout = true;
}

void WindowCompositor::on_resize(render::Extent2D physical_size)
{
    if (render::is_empty(physical_size))
    {
        return; // a minimized window reports 0x0 every frame — keep the last valid configuration
    }
    if (physical_size.width == size_.width && physical_size.height == size_.height)
    {
        return;
    }
    size_ = physical_size;
    if (swapchain_ != nullptr)
    {
        swapchain_->resize(physical_size);
    }
    damage_.resize = true;
}

void WindowCompositor::reconfigure()
{
    if (surface_ == nullptr || device_ == nullptr || render::is_empty(size_))
    {
        return;
    }
    ++stats_.reconfigures;
    if (swapchain_ != nullptr)
    {
        swapchain_->resize(size_);
        return;
    }
    render::SurfaceConfig config;
    config.size = size_;
    config.format = render::TextureFormat::BGRA8Unorm;
    config.present_mode = render::PresentMode::Fifo;
    swapchain_ = surface_->configure(*device_, config);
}

bool WindowCompositor::draw_layer(render::IRenderPassEncoder& pass, const render::Rect2D& dst_rect,
                                  render::ITextureView& view, const render::Rect2D& visible_rect,
                                  render::Extent2D coded_size, std::size_t slot)
{
    const render::Rect2D clipped = clip_to(dst_rect, size_);
    if (render::is_empty(clipped.size))
    {
        return false;
    }
    const CompositeUv uv = compute_layer_uv(clipped, size_, visible_rect, coded_size);

    // One uniform buffer + bind group PER SLOT — see the member declaration for why they cannot be
    // shared across draws inside one pass.
    while (layer_uniforms_.size() <= slot)
    {
        render::BufferDesc desc;
        desc.size = sizeof(CompositeUv);
        desc.uniform = true;
        desc.copy_dst = true;
        layer_uniforms_.push_back(device_->create_buffer(desc));
        layer_bind_groups_.push_back(nullptr);
    }
    if (layer_uniforms_[slot] == nullptr)
    {
        return false;
    }
    device_->queue().write_buffer(*layer_uniforms_[slot], 0, &uv, sizeof(uv));

    std::vector<render::BindGroupEntry> entries(3);
    entries[0].binding = 0;
    entries[0].buffer = layer_uniforms_[slot].get();
    entries[0].buffer_size = sizeof(CompositeUv);
    entries[1].binding = 1;
    entries[1].texture = &view;
    entries[2].binding = 2;
    entries[2].sampler = sampler_.get();
    layer_bind_groups_[slot] = device_->create_bind_group(*bind_layout_, entries);
    if (layer_bind_groups_[slot] == nullptr)
    {
        return false;
    }

    // The scissor is set for EVERY layer, including the full-window one: scissor state persists
    // across draws within a pass, so a full-window layer drawn after a scissored viewport layer
    // would otherwise inherit the viewport's rect and be clipped to it.
    pass.set_scissor_rect(clipped.origin, clipped.size);
    pass.set_pipeline(*pipeline_);
    pass.set_bind_group(0, *layer_bind_groups_[slot]);
    pass.draw(render::present::kCompositeVertexCount, 1);
    return true;
}

bool WindowCompositor::render_gpu_frame()
{
    if (swapchain_ == nullptr && surface_ != nullptr)
    {
        reconfigure();
    }
    if (swapchain_ == nullptr || !ensure_gpu_resources())
    {
        return false;
    }

    const render::AcquiredFrame acquired = swapchain_->acquire();
    switch (acquired.status)
    {
    case render::AcquireStatus::Ok:
        break;
    case render::AcquireStatus::Outdated:
    case render::AcquireStatus::Lost:
        // NORMAL, not errors (e03): a resize raced the frame, or the device went away. Reconfigure
        // and let the next frame draw; the damage is deliberately NOT cleared by the caller.
        ++stats_.acquire_failures;
        reconfigure();
        return false;
    case render::AcquireStatus::Timeout:
    case render::AcquireStatus::Error:
    default:
        ++stats_.acquire_failures;
        return false;
    }
    if (acquired.view == nullptr)
    {
        ++stats_.acquire_failures;
        return false;
    }

    std::unique_ptr<render::ICommandEncoder> encoder = device_->create_command_encoder();
    render::RenderPassDesc pass_desc;
    render::ColorAttachment attachment;
    attachment.view = acquired.view;
    attachment.load = render::LoadOp::Clear;
    attachment.clear = config_.clear;
    pass_desc.color.push_back(attachment);
    std::unique_ptr<render::IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);

    // The OSR layers' texture views are owned HERE, as locals that outlive the pass: a bind group
    // references its view, so the view must stay alive until end(). Keeping them in compositor
    // members would mean tracking which slot owns what, and a layer whose draw was skipped (a rect
    // that clipped to nothing) would leave that bookkeeping out of step with the slots.
    std::unique_ptr<render::ITextureView> view_layer;
    std::unique_ptr<render::ITextureView> popup_layer;

    std::size_t slot = 0;
    // 2. viewport layers first — the CEF layer composites OVER them through its transparent holes.
    for (const ViewportLayer& layer : viewports_)
    {
        if (layer.content == nullptr || render::is_empty(layer.content_size))
        {
            continue;
        }
        const render::Rect2D whole{render::Origin2D{}, layer.content_size};
        if (draw_layer(*pass, layer.content_rect, *layer.content, whole, layer.content_size, slot))
        {
            ++slot;
            ++stats_.viewport_draws;
        }
    }

    // 3. the full-window CEF layer.
    if (have_view_frame_ && view_importer_.texture() != nullptr)
    {
        view_layer = view_importer_.texture()->create_view();
        if (view_layer != nullptr)
        {
            const render::Rect2D whole{render::Origin2D{}, size_};
            if (draw_layer(*pass, whole, *view_layer, view_visible_rect_, view_coded_size_, slot))
            {
                ++slot;
            }
        }
    }

    // 4. the PET_POPUP layer — a SECOND OSR layer, confined to the popup rect.
    if (popup_visible_ && have_popup_frame_ && popup_importer_.texture() != nullptr &&
        !render::is_empty(popup_rect_.size))
    {
        popup_layer = popup_importer_.texture()->create_view();
        if (popup_layer != nullptr)
        {
            if (draw_layer(*pass, popup_rect_, *popup_layer, popup_visible_rect_, popup_coded_size_,
                           slot))
            {
                ++slot;
                ++stats_.popup_draws;
            }
        }
    }

    pass->end();
    std::unique_ptr<render::ICommandBuffer> commands = encoder->finish();
    device_->queue().submit(*commands);
    swapchain_->present();

    // Suboptimal means the frame WAS presentable and had to be presented — then reconfigured. This
    // is the one status where presenting first and reconfiguring after is required; treating it like
    // Outdated would drop a good frame every time a resize is pending.
    if (acquired.suboptimal)
    {
        reconfigure();
    }
    return true;
}

bool WindowCompositor::render_cpu_frame()
{
    if (render::is_empty(size_))
    {
        return false;
    }
    if (!have_view_frame_ || !cpu_view_.valid)
    {
        return false;
    }

    // Compose into a tightly-packed copy of the view frame, then blit. The popup is composited HERE
    // rather than skipped, so the GPU-less fallback is not silently popup-less.
    const std::uint32_t stride = cpu_view_.coded_size.width * 4u;
    cpu_surface_.assign(static_cast<std::size_t>(stride) * cpu_view_.coded_size.height, 0u);
    for (std::uint32_t y = 0; y < cpu_view_.coded_size.height; ++y)
    {
        const std::uint8_t* src =
            cpu_view_.pixels.data() + static_cast<std::size_t>(y) * cpu_view_.bytes_per_row;
        std::uint8_t* dst = cpu_surface_.data() + static_cast<std::size_t>(y) * stride;
        std::memcpy(dst, src, stride);
    }

    if (popup_visible_ && have_popup_frame_ && cpu_popup_.valid &&
        !render::is_empty(popup_rect_.size))
    {
        // The popup's VISIBLE rect inside its own allocation is what gets drawn, at the popup rect's
        // origin in the view. Using the coded size here would composite the allocation's unused
        // margin as if it were content.
        const render::Extent2D popup_size{
            std::min(popup_rect_.size.width, cpu_popup_.visible_rect.size.width == 0
                                                 ? cpu_popup_.coded_size.width
                                                 : cpu_popup_.visible_rect.size.width),
            std::min(popup_rect_.size.height, cpu_popup_.visible_rect.size.height == 0
                                                  ? cpu_popup_.coded_size.height
                                                  : cpu_popup_.visible_rect.size.height)};
        blend_premultiplied_bgra(cpu_surface_.data(), cpu_view_.coded_size, stride,
                                 cpu_popup_.pixels.data(), popup_size, cpu_popup_.bytes_per_row,
                                 popup_rect_.origin);
        ++stats_.popup_draws;
    }

    if (blitter_ == nullptr)
    {
        return false; // reported at attach; the shell still runs
    }
    render::present::BlitImage image;
    image.pixels = cpu_surface_.data();
    image.byte_size = cpu_surface_.size();
    image.size = cpu_view_.coded_size;
    image.bytes_per_row = stride;
    return blitter_->blit(image, size_);
}

bool WindowCompositor::render_frame()
{
    ++stats_.frames_attempted;
    if (!damage_.any())
    {
        ++stats_.frames_skipped_no_damage;
        return false;
    }

    bool presented = false;
    switch (path_)
    {
    case PresentPath::gpu_swapchain:
        presented = render_gpu_frame();
        break;
    case PresentPath::cpu_blit:
        presented = render_cpu_frame();
        break;
    case PresentPath::none:
    default:
        break;
    }

    if (presented)
    {
        ++stats_.frames_presented;
        damage_.clear();
    }
    // On a FAILED frame the damage deliberately STAYS set: an Outdated acquire reconfigured and the
    // next frame must draw the content that was never presented. Clearing here would blank the
    // editor until something else happened to damage it — which, in a damage-driven shell that has
    // just gone idle, can be never.
    return presented;
}

} // namespace context::editor::shell
