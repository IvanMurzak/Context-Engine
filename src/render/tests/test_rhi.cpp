// RHI abstraction coherence test: drive the FULL rhi.h object model
// (device/queue/pipeline/buffer/texture/command-encoding) end to end through the offscreen render +
// pixel-readback proof, against the GPU-free fake backend. Because the SAME render_offscreen_triangle
// code drives the real wgpu-native backend in CI, a green here means the abstraction is coherent and
// usable, and the real backend only has to implement rhi.h correctly to pass the same assertions.

#include "context/render/offscreen_scene.h"
#include "context/render/rhi.h"

#include <cstring>
#include <vector>

#include "render_test.h"
#include "render_test_rhi.h"

using namespace context::render;

namespace
{

void test_offscreen_readback_through_abstraction()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    const AdapterProbe probe = rhi.probe();
    CHECK(probe.has_adapter);

    std::unique_ptr<IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    if (device == nullptr)
    {
        return;
    }

    const OffscreenResult result = render_offscreen_triangle(*device);
    CHECK(result == OffscreenResult::Pass);
}

void test_clear_only_without_pipeline()
{
    // A render pass with no pipeline/draw still clears the target — the buffer reads back the clear
    // color everywhere (exercises the command-encoding + copy + map path with no draw).
    rendertest::FakeRhi rhi(1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    if (device == nullptr)
    {
        return;
    }

    TextureDesc td;
    td.size = {4, 4};
    td.format = TextureFormat::RGBA8Unorm;
    td.render_attachment = true;
    td.copy_src = true;
    std::unique_ptr<ITexture> texture = device->create_texture(td);
    std::unique_ptr<ITextureView> view = texture->create_view();

    BufferDesc bd;
    bd.size = 4u * 4u * 4u;
    bd.copy_dst = true;
    bd.map_read = true;
    std::unique_ptr<IBuffer> buffer = device->create_buffer(bd);

    std::unique_ptr<ICommandEncoder> encoder = device->create_command_encoder();
    RenderPassDesc pass_desc;
    ColorAttachment attach;
    attach.view = view.get();
    attach.load = LoadOp::Clear;
    attach.clear = Color{1.0, 0.0, 0.0, 1.0}; // red clear
    pass_desc.color.push_back(attach);
    {
        std::unique_ptr<IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);
        pass->end();
    }
    TexelCopyBufferLayout layout;
    layout.bytes_per_row = 4u * 4u;
    layout.rows_per_image = 4u;
    encoder->copy_texture_to_buffer(*texture, *buffer, layout, {4, 4});
    std::unique_ptr<ICommandBuffer> commands = encoder->finish();
    device->queue().submit(*commands);

    const auto* px = static_cast<const std::uint8_t*>(buffer->map_read());
    CHECK(px != nullptr);
    if (px != nullptr)
    {
        CHECK(px[0] == 255); // r
        CHECK(px[1] == 0);   // g
        CHECK(px[2] == 0);   // b
        CHECK(px[3] == 255); // a
    }
    buffer->unmap();
}

// The lit-path RHI surface (R-REND-004/006): depth textures + depth-only pipelines, samplers
// (comparison + filtering), reflected bind-group layouts + bind groups, and the queue writes —
// driven end to end on the fake backend, so the abstraction the real wgpu backend implements for
// the CI lit proof is locally proven coherent (the sibling of the offscreen-triangle coverage).
void test_lit_surface_object_model()
{
    rendertest::FakeRhi rhi(1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    if (device == nullptr)
    {
        return;
    }

    // A shadow-map-shaped depth texture: render target + sampled binding.
    TextureDesc depth_desc;
    depth_desc.size = {8, 8};
    depth_desc.format = TextureFormat::Depth32Float;
    depth_desc.render_attachment = true;
    depth_desc.texture_binding = true;
    std::unique_ptr<ITexture> depth_tex = device->create_texture(depth_desc);
    std::unique_ptr<ITextureView> depth_view = depth_tex->create_view();
    CHECK(depth_view != nullptr);

    // A uniform buffer written through the queue (the extract->uniform path).
    BufferDesc uniform_desc;
    uniform_desc.size = 64;
    uniform_desc.uniform = true;
    uniform_desc.copy_dst = true;
    std::unique_ptr<IBuffer> uniform = device->create_buffer(uniform_desc);
    float values[4] = {1.5f, -2.0f, 0.25f, 4.0f};
    device->queue().write_buffer(*uniform, 16, values, sizeof values);
    {
        auto& fake = static_cast<rendertest::FakeBuffer&>(*uniform);
        float readback[4] = {};
        std::memcpy(readback, fake.bytes().data() + 16, sizeof readback);
        CHECK(readback[0] == 1.5f && readback[1] == -2.0f && readback[3] == 4.0f);
    }

    // A texel upload through the queue (the lightmap INPUT hook path, R-REND-006).
    TextureDesc lm_desc;
    lm_desc.size = {2, 2};
    lm_desc.format = TextureFormat::RGBA8Unorm;
    lm_desc.texture_binding = true;
    lm_desc.copy_dst = true;
    std::unique_ptr<ITexture> lightmap = device->create_texture(lm_desc);
    const std::uint8_t texels[16] = {64, 51, 26, 255, 64, 51, 26, 255,
                                     64, 51, 26, 255, 64, 51, 26, 255};
    TexelCopyBufferLayout lm_layout;
    lm_layout.bytes_per_row = 8;
    lm_layout.rows_per_image = 2;
    device->queue().write_texture(*lightmap, texels, sizeof texels, lm_layout, {2, 2});
    {
        auto& fake = static_cast<rendertest::FakeTexture&>(*lightmap);
        CHECK(fake.pixels()[0] == 64 && fake.pixels()[1] == 51 && fake.pixels()[15] == 255);
    }
    std::unique_ptr<ITextureView> lightmap_view = lightmap->create_view();
    CHECK(lightmap_view != nullptr);

    // Samplers: a PCF comparison sampler and a plain filtering sampler.
    SamplerDesc compare_desc;
    compare_desc.min_filter = FilterMode::Linear;
    compare_desc.mag_filter = FilterMode::Linear;
    compare_desc.compare = CompareFunction::LessEqual;
    std::unique_ptr<ISampler> compare_sampler = device->create_sampler(compare_desc);
    std::unique_ptr<ISampler> plain_sampler = device->create_sampler(SamplerDesc{});
    CHECK(static_cast<rendertest::FakeSampler&>(*compare_sampler).desc().compare ==
          CompareFunction::LessEqual);
    CHECK(static_cast<rendertest::FakeSampler&>(*plain_sampler).desc().compare ==
          CompareFunction::Undefined);

    // A depth-only pipeline (empty fragment entry) with a depth state — the shadow-pass shape.
    RenderPipelineDesc shadow_desc;
    shadow_desc.wgsl = "// depth-only";
    shadow_desc.vertex_entry = "vs_shadow";
    shadow_desc.fragment_entry = "";
    shadow_desc.depth = DepthState{TextureFormat::Depth32Float, true, CompareFunction::Less};
    std::unique_ptr<IRenderPipeline> shadow_pipe = device->create_render_pipeline(shadow_desc);
    CHECK(static_cast<rendertest::FakePipeline&>(*shadow_pipe).desc().depth.has_value());
    CHECK(static_cast<rendertest::FakePipeline&>(*shadow_pipe).desc().fragment_entry.empty());

    // Bind groups against the pipeline's REFLECTED layout (the rhi.h auto-layout model).
    std::unique_ptr<IBindGroupLayout> layout = shadow_pipe->bind_group_layout(0);
    CHECK(layout != nullptr);
    std::vector<BindGroupEntry> entries(3);
    entries[0].binding = 0;
    entries[0].buffer = uniform.get();
    entries[1].binding = 1;
    entries[1].texture = depth_view.get();
    entries[2].binding = 2;
    entries[2].sampler = compare_sampler.get();
    std::unique_ptr<IBindGroup> bind_group = device->create_bind_group(*layout, entries);
    CHECK(bind_group != nullptr);
    CHECK(static_cast<rendertest::FakeBindGroup&>(*bind_group).entries().size() == 3u);

    // A depth-only render pass (no color attachment) records set_bind_group + draw and finishes.
    std::unique_ptr<ICommandEncoder> encoder = device->create_command_encoder();
    RenderPassDesc pass_desc;
    DepthAttachment depth_attach;
    depth_attach.view = depth_view.get();
    depth_attach.clear_depth = 1.0f;
    pass_desc.depth = depth_attach;
    {
        std::unique_ptr<IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);
        pass->set_pipeline(*shadow_pipe);
        pass->set_bind_group(0, *bind_group);
        pass->draw(12, 1);
        pass->end();
    }
    std::unique_ptr<ICommandBuffer> commands = encoder->finish();
    device->queue().submit(*commands);
}

} // namespace

int main()
{
    test_offscreen_readback_through_abstraction();
    test_clear_only_without_pipeline();
    test_lit_surface_object_model();
    RENDER_TEST_MAIN_END();
}
