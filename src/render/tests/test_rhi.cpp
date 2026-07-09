// RHI abstraction coherence test: drive the FULL rhi.h object model
// (device/queue/pipeline/buffer/texture/command-encoding) end to end through the offscreen render +
// pixel-readback proof, against the GPU-free fake backend. Because the SAME render_offscreen_triangle
// code drives the real wgpu-native backend in CI, a green here means the abstraction is coherent and
// usable, and the real backend only has to implement rhi.h correctly to pass the same assertions.

#include "context/render/offscreen_scene.h"
#include "context/render/rhi.h"

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

} // namespace

int main()
{
    test_offscreen_readback_through_abstraction();
    test_clear_only_without_pipeline();
    RENDER_TEST_MAIN_END();
}
