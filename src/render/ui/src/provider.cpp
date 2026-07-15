// The engine-integrated GPU UI backend — see context/render/ui/provider.h.

#include "context/render/ui/provider.h"

#include "context/render/sprite/ortho.h"
#include "context/render/sprite/sprite_offscreen.h" // sprite::quad_wgsl
#include "context/render/ui/batch.h"

#include <array>

namespace context::render::ui
{

namespace
{

// The UI surface projection: a screen-space ortho camera mapping surface pixels (y-DOWN, top-left
// origin — the a1 Rect convention) into WebGPU clip space. The camera frames [0,w] x [0,h] in a y-UP
// world; a UI rect's y is flipped into that world so a rect at surface (x,y) lands at readback row y
// (row 0 = top), matching the analytic golden + every other corpus scene's readback orientation.
sprite::Mat4 surface_projection(Extent2D surface)
{
    sprite::Camera2D cam;
    cam.center = {static_cast<float>(surface.width) * 0.5f, static_cast<float>(surface.height) * 0.5f};
    cam.half_width = static_cast<float>(surface.width) * 0.5f;
    cam.half_height = static_cast<float>(surface.height) * 0.5f;
    return cam.projection();
}

// Surface-space rect (y-down) -> y-up world quad corners for the ortho projection.
std::array<sprite::Vec2, 4> quad_corners(const sprite::Mat4& proj, const packages::ui::Rect& rect,
                                         float surface_height)
{
    const sprite::Vec2 center{rect.x + rect.w * 0.5f, surface_height - (rect.y + rect.h * 0.5f)};
    return sprite::quad_clip_corners(proj, center, sprite::Vec2{rect.w, rect.h});
}

} // namespace

GpuUiProvider::GpuUiProvider(IDevice& device, Extent2D surface, Color clear)
    : device_(device), surface_(surface), clear_(clear)
{
}

GpuUiProvider::GpuUiProvider(IDevice& device, ITexture& target, Extent2D surface, Color clear)
    : device_(device), surface_(surface), clear_(clear), external_target_(&target)
{
}

packages::ui::Capabilities GpuUiProvider::capabilities() const
{
    packages::ui::Capabilities caps;
    caps.gpu_driver = true;            // GPU-driven presentation
    caps.damage_repaint = true;        // repaints dirty regions only
    caps.composited_transforms = true; // transform/opacity fold at composite time, no relayout
    caps.text_shaping = true;          // a8: HarfBuzz-class shaping (context_ui_text::measure, headless)
    caps.bidi = true;                  // a8: UAX #9 bidi + UAX #24 itemization (SheenBidi)
    // ime stays false — no OS text-entry integration in M7 exit scope (declared capability, owner O-2).
    return caps;
}

void GpuUiProvider::ensure_layer()
{
    if (external_target_ != nullptr || layer_ != nullptr)
    {
        return; // external target is owned by the caller; a self-owned layer is allocated once + reused
    }
    TextureDesc desc;
    desc.size = surface_;
    desc.format = TextureFormat::RGBA8Unorm;
    desc.render_attachment = true; // rendered into each frame
    desc.copy_src = true;          // read back for the golden dump / CI proof
    desc.texture_binding = true;   // sampled by the (deferred) full-screen composite-onto-backbuffer pass
    layer_ = device_.create_texture(desc);
}

void GpuUiProvider::repaint(const std::vector<std::uint32_t>& draw_set, bool clear)
{
    const UiRenderSnapshot& snap = buffers_.front();
    const sprite::Mat4 proj = surface_projection(surface_);
    const float surface_h = static_cast<float>(surface_.height);
    std::unique_ptr<ITextureView> view = active_layer()->create_view();

    // A Clear repaint with no quads still issues one clear pass so the layer is wiped (an emptied UI);
    // a Load repaint with no damaged quads leaves the persistent layer untouched (nothing to redraw).
    if (draw_set.empty())
    {
        if (!clear)
        {
            return;
        }
        std::unique_ptr<ICommandEncoder> encoder = device_.create_command_encoder();
        RenderPassDesc pass_desc;
        ColorAttachment attach;
        attach.view = view.get();
        attach.load = LoadOp::Clear;
        attach.store = StoreOp::Store;
        attach.clear = clear_;
        pass_desc.color.push_back(attach);
        encoder->begin_render_pass(pass_desc)->end();
        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device_.queue().submit(*commands);
        return;
    }

    // Each quad is its own render pass into the same layer (mirrors sprite_offscreen.h — the T1 RHI has
    // no vertex buffers yet, so geometry is baked per-quad). The FIRST pass carries the requested LoadOp
    // (Clear for a full repaint, Load to preserve the persistent layer on a damage repaint); every later
    // pass LOADs so quads composite on top in draw (painter) order.
    bool first = true;
    for (const std::uint32_t idx : draw_set)
    {
        const UiQuad& quad = snap.quads[idx];
        const std::array<sprite::Vec2, 4> corners = quad_corners(proj, quad.rect, surface_h);
        const float color[4] = {static_cast<float>(quad.color.r) / 255.0f,
                                static_cast<float>(quad.color.g) / 255.0f,
                                static_cast<float>(quad.color.b) / 255.0f,
                                static_cast<float>(quad.color.a) / 255.0f};

        RenderPipelineDesc pipe_desc;
        pipe_desc.wgsl = sprite::quad_wgsl(corners, color);
        pipe_desc.color_format = TextureFormat::RGBA8Unorm;
        std::unique_ptr<IRenderPipeline> pipeline = device_.create_render_pipeline(pipe_desc);

        std::unique_ptr<ICommandEncoder> encoder = device_.create_command_encoder();
        RenderPassDesc pass_desc;
        ColorAttachment attach;
        attach.view = view.get();
        attach.load = (first && clear) ? LoadOp::Clear : LoadOp::Load;
        attach.store = StoreOp::Store;
        attach.clear = clear_;
        pass_desc.color.push_back(attach);
        {
            std::unique_ptr<IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);
            pass->set_pipeline(*pipeline);
            pass->draw(6, 1);
            pass->end();
        }
        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device_.queue().submit(*commands);
        first = false;
    }
}

void GpuUiProvider::present(const packages::ui::UiTree& tree, const packages::ui::RepaintPlan& plan)
{
    ensure_layer();
    const packages::ui::Rect viewport{0.0f, 0.0f, static_cast<float>(surface_.width),
                                      static_cast<float>(surface_.height)};
    buffers_.extract(tree, viewport);

    // First frame always clears (the persistent layer starts undefined); afterwards a full repaint
    // clears and a damage repaint LOADs + redraws only the overlapping quads.
    const bool clear = plan.full_repaint || frames_presented_ == 0;
    const std::vector<std::uint32_t> draw_set = select_draw_set(buffers_.front(), plan);
    repaint(draw_set, clear);

    last_draw_count_ = draw_set.size();
    last_was_clear_ = clear;
    ++frames_presented_;
}

bool GpuUiProvider::read_layer(std::vector<std::uint8_t>& out)
{
    ITexture* layer = active_layer();
    if (layer == nullptr)
    {
        return false;
    }
    constexpr std::uint32_t kBpp = 4;
    const std::uint32_t bytes_per_row = surface_.width * kBpp; // 256-wide targets are 256-aligned
    BufferDesc bd;
    bd.size = static_cast<std::uint64_t>(bytes_per_row) * surface_.height;
    bd.copy_dst = true;
    bd.map_read = true;
    std::unique_ptr<IBuffer> readback = device_.create_buffer(bd);

    std::unique_ptr<ICommandEncoder> encoder = device_.create_command_encoder();
    TexelCopyBufferLayout layout;
    layout.bytes_per_row = bytes_per_row;
    layout.rows_per_image = surface_.height;
    encoder->copy_texture_to_buffer(*layer, *readback, layout, surface_);
    std::unique_ptr<ICommandBuffer> commands = encoder->finish();
    device_.queue().submit(*commands);

    const auto* pixels = static_cast<const std::uint8_t*>(readback->map_read());
    if (pixels == nullptr)
    {
        return false;
    }
    out.assign(pixels, pixels + static_cast<std::size_t>(bytes_per_row) * surface_.height);
    readback->unmap();
    return true;
}

} // namespace context::render::ui
