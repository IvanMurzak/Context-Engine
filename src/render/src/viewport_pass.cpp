// The viewport render pass -- see context/render/viewport_pass.h for the design, the proxy-geometry
// ground truth, and the signature note.

#include "context/render/viewport_pass.h"

#include "context/render/math.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace context::render
{
namespace
{

constexpr const char* kViewportWgsl = R"WGSL(
struct Draw {
    mvp : mat4x4<f32>,
    color : vec4<f32>,
    shade : vec4<f32>,
};

@group(0) @binding(0) var<uniform> draw : Draw;

struct VsOut {
    @builtin(position) position : vec4<f32>,
    @location(0) tint : vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex_index : u32) -> VsOut {
    // A unit box centred on the model origin, spanning [-0.5, 0.5] on every axis. Eight corners
    // indexed by a 36-entry triangle list (two triangles per face) -- a grid line is this same box
    // scaled to a sliver on two axes, so one shader draws both the grid and the proxies.
    var corners = array<vec3<f32>, 8>(
        vec3<f32>(-0.5, -0.5, -0.5),
        vec3<f32>( 0.5, -0.5, -0.5),
        vec3<f32>( 0.5,  0.5, -0.5),
        vec3<f32>(-0.5,  0.5, -0.5),
        vec3<f32>(-0.5, -0.5,  0.5),
        vec3<f32>( 0.5, -0.5,  0.5),
        vec3<f32>( 0.5,  0.5,  0.5),
        vec3<f32>(-0.5,  0.5,  0.5));
    var indices = array<u32, 36>(
        0u, 2u, 1u, 0u, 3u, 2u,
        4u, 5u, 6u, 4u, 6u, 7u,
        0u, 4u, 7u, 0u, 7u, 3u,
        1u, 2u, 6u, 1u, 6u, 5u,
        0u, 1u, 5u, 0u, 5u, 4u,
        3u, 7u, 6u, 3u, 6u, 2u);
    let local = corners[indices[vertex_index]];
    var out : VsOut;
    out.position = draw.mvp * vec4<f32>(local, 1.0);
    // Flat per-FACE shading so a proxy box reads as a box rather than a silhouette. shade.x = 0
    // (every grid line) leaves the colour untouched.
    let face = f32(vertex_index / 6u);
    let f = 1.0 - draw.shade.x * (face / 5.0);
    out.tint = vec4<f32>(draw.color.rgb * f, draw.color.a);
    return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4<f32> {
    return in.tint;
}
)WGSL";

[[nodiscard]] bool finite(float v)
{
    return std::isfinite(v);
}

// Whether every component of a snapshot Transform is finite. A single NaN or infinity flows through
// the model matrix into the view-projection and poisons the whole draw, so the item is skipped
// instead -- the same "stay finite" house rule math.h's own degenerate guards follow.
[[nodiscard]] bool transform_is_finite(const Transform& t)
{
    for (std::size_t i = 0; i < 3; ++i)
    {
        if (!finite(t.position[i]) || !finite(t.scale[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < 4; ++i)
    {
        if (!finite(t.rotation[i]))
        {
            return false;
        }
    }
    return true;
}

// A model matrix that scales the unit box by `s` then translates it to `t` (no rotation) -- what a
// grid line needs.
[[nodiscard]] Mat4 translate_scale(Vec3 t, Vec3 s)
{
    Mat4 m;
    m.m[0] = s.x;
    m.m[5] = s.y;
    m.m[10] = s.z;
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}

// The model matrix of a proxy: translate * rotate * scale, with the item's scale multiplied by the
// configured proxy edge. Built directly rather than through three mul()s because the composition is
// exactly "scale the rotation's columns, then drop the translation into the last one".
[[nodiscard]] Mat4 proxy_model(const Transform& transform, float proxy_size)
{
    Mat4 m = rotation_from_quaternion(transform.rotation[0], transform.rotation[1],
                                      transform.rotation[2], transform.rotation[3]);
    for (std::size_t c = 0; c < 3; ++c)
    {
        const float s = transform.scale[c] * proxy_size;
        for (std::size_t r = 0; r < 3; ++r)
        {
            m.m[c * 4 + r] *= s;
        }
    }
    m.m[12] = transform.position[0];
    m.m[13] = transform.position[1];
    m.m[14] = transform.position[2];
    m.m[15] = 1.0f;
    return m;
}

// The palette slot a grid line at `index` off the origin takes. `along_x` selects which axis line
// the index-0 line IS: the line parallel to X at z = 0 is the X axis, and vice versa.
[[nodiscard]] Color grid_line_color(const ViewportGridConfig& grid, std::int32_t index, bool along_x)
{
    if (index == 0)
    {
        return along_x ? grid.axis_x : grid.axis_z;
    }
    const std::uint32_t every = grid.major_every == 0u ? 1u : grid.major_every;
    const std::uint32_t magnitude = static_cast<std::uint32_t>(index < 0 ? -index : index);
    return (magnitude % every == 0u) ? grid.major : grid.minor;
}

// One recorded draw, built before the pass so every uniform upload lands ahead of the submit.
struct PendingDraw
{
    Mat4 model;
    Color color;
    float shade = 0.0f;
    bool is_grid = false;
};

} // namespace

const char* viewport_pass_wgsl()
{
    return kViewportWgsl;
}

RenderPipelineDesc make_viewport_pipeline_desc(TextureFormat color_format, bool depth)
{
    RenderPipelineDesc desc;
    desc.wgsl = viewport_pass_wgsl();
    desc.color_format = color_format;
    desc.topology = PrimitiveTopology::TriangleList;
    if (depth)
    {
        DepthState state;
        state.format = TextureFormat::Depth32Float;
        state.depth_write = true;
        state.depth_compare = CompareFunction::Less;
        desc.depth = state;
    }
    return desc;
}

ViewportFrameStats render_viewport_view(IDevice& device, const View& view,
                                        const RenderSnapshot& snapshot, ITextureView& target,
                                        Extent2D target_size, const ViewportPassConfig& config)
{
    ViewportFrameStats stats;
    if (is_empty(target_size))
    {
        // A viewport panel mid-resize legitimately reports a zero extent. Recording a pass against it
        // would ask the backend for a zero-area attachment.
        return stats;
    }

    const Mat4 view_projection = view_proj(view, target_size);

    std::vector<PendingDraw> pending;

    // --- the grid, first, so the proxies depth-test over it ------------------------------------
    if (config.draw_grid)
    {
        const ViewportGridConfig& grid = config.grid;
        const std::int32_t half = static_cast<std::int32_t>(grid.half_lines);
        const float span = 2.0f * static_cast<float>(grid.half_lines) * grid.spacing;
        pending.reserve(static_cast<std::size_t>(grid_line_count(grid)) + snapshot.items.size());

        // Lines PARALLEL TO X (varying z) first, then lines parallel to Z, each from -half to +half.
        // The order is fixed so a caller (and a headless test) can address a specific line by index.
        for (std::int32_t i = -half; i <= half; ++i)
        {
            PendingDraw draw;
            draw.model = translate_scale(Vec3{0.0f, 0.0f, static_cast<float>(i) * grid.spacing},
                                         Vec3{span, grid.line_width, grid.line_width});
            draw.color = grid_line_color(grid, i, /*along_x=*/true);
            draw.is_grid = true;
            pending.push_back(draw);
        }
        for (std::int32_t i = -half; i <= half; ++i)
        {
            PendingDraw draw;
            draw.model = translate_scale(Vec3{static_cast<float>(i) * grid.spacing, 0.0f, 0.0f},
                                         Vec3{grid.line_width, grid.line_width, span});
            draw.color = grid_line_color(grid, i, /*along_x=*/false);
            draw.is_grid = true;
            pending.push_back(draw);
        }
    }
    else
    {
        pending.reserve(snapshot.items.size());
    }

    // --- one proxy box per renderable, at its AUTHORED transform -------------------------------
    for (const RenderItem& item : snapshot.items)
    {
        if (!transform_is_finite(item.transform))
        {
            ++stats.skipped_items;
            continue;
        }
        PendingDraw draw;
        draw.model = proxy_model(item.transform, config.proxy_size);
        draw.color = Color{static_cast<double>(item.renderable.color[0]),
                           static_cast<double>(item.renderable.color[1]),
                           static_cast<double>(item.renderable.color[2]),
                           static_cast<double>(item.renderable.color[3])};
        draw.shade = config.proxy_shade;
        pending.push_back(draw);
    }

    // --- device resources ----------------------------------------------------------------------
    std::unique_ptr<IBuffer> uniforms;
    std::unique_ptr<IRenderPipeline> pipeline;
    std::unique_ptr<IBindGroupLayout> layout;
    std::vector<std::unique_ptr<IBindGroup>> groups;

    if (!pending.empty())
    {
        BufferDesc buffer_desc;
        buffer_desc.size = kViewportDrawUniformStride * static_cast<std::uint64_t>(pending.size());
        buffer_desc.uniform = true;
        buffer_desc.copy_dst = true;
        uniforms = device.create_buffer(buffer_desc);

        pipeline = device.create_render_pipeline(
            make_viewport_pipeline_desc(config.color_format, config.depth != nullptr));
        if (uniforms == nullptr || pipeline == nullptr)
        {
            return stats; // fail closed rather than record a pass that draws nothing
        }
        layout = pipeline->bind_group_layout(0);
        if (layout == nullptr)
        {
            return stats;
        }

        groups.reserve(pending.size());
        for (std::size_t i = 0; i < pending.size(); ++i)
        {
            const PendingDraw& draw = pending[i];
            const Mat4 mvp = mul(view_projection, draw.model);

            ViewportDrawUniform block{};
            for (std::size_t k = 0; k < 16; ++k)
            {
                block.mvp[k] = mvp.m[k];
            }
            block.color[0] = static_cast<float>(draw.color.r);
            block.color[1] = static_cast<float>(draw.color.g);
            block.color[2] = static_cast<float>(draw.color.b);
            block.color[3] = static_cast<float>(draw.color.a);
            block.shade[0] = draw.shade;

            const std::uint64_t offset = kViewportDrawUniformStride * static_cast<std::uint64_t>(i);
            device.queue().write_buffer(*uniforms, offset, &block, sizeof(block));

            std::vector<BindGroupEntry> entries(1);
            entries[0].binding = 0;
            entries[0].buffer = uniforms.get();
            entries[0].buffer_offset = offset;
            entries[0].buffer_size = sizeof(ViewportDrawUniform);
            groups.push_back(device.create_bind_group(*layout, entries));
        }
    }

    // --- record + submit ------------------------------------------------------------------------
    std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
    if (encoder == nullptr)
    {
        return stats;
    }

    RenderPassDesc pass_desc;
    ColorAttachment color;
    color.view = &target;
    color.load = LoadOp::Clear;
    color.store = StoreOp::Store;
    color.clear = config.clear;
    pass_desc.color.push_back(color);
    if (config.depth != nullptr)
    {
        DepthAttachment depth;
        depth.view = config.depth;
        depth.load = LoadOp::Clear;
        depth.store = StoreOp::Store;
        depth.clear_depth = 1.0f;
        pass_desc.depth = depth;
    }

    {
        std::unique_ptr<IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);
        if (pass == nullptr)
        {
            return stats;
        }
        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            if (groups[i] == nullptr)
            {
                continue; // the device refused this bind group; it is not counted as drawn
            }
            pass->set_pipeline(*pipeline);
            pass->set_bind_group(0, *groups[i]);
            pass->draw(kViewportBoxVertexCount, 1);
            if (pending[i].is_grid)
            {
                ++stats.grid_draws;
            }
            else
            {
                ++stats.proxy_draws;
            }
        }
        pass->end();
    }

    std::unique_ptr<ICommandBuffer> commands = encoder->finish();
    if (commands == nullptr)
    {
        return stats;
    }
    device.queue().submit(*commands);
    stats.rendered = true;
    return stats;
}

} // namespace context::render
