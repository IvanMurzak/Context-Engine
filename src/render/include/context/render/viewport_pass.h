// The viewport render pass (M9 e11b; design m9-editor D5) -- what one editor viewport actually draws
// through the e11a Camera/View: the composed scene's PROXY GEOMETRY at its authored transforms, over
// the ground grid.
//
// ⚠ PROXY GEOMETRY IS THE WHOLE STORY, and it is a property of the DATA, not a shortcut taken here.
// Two facts in the tree bound what a scene can even ask to be drawn, and the follow-up work
// inherits both rather than rediscovering them:
//
//   * The `ctx:scene` schema (kSceneSchemaJson, src/editor/schema/src/kind_schema.cpp) declares
//     exactly TWO components. `transform` is `additionalProperties:false` with `required:["position"]`
//     and carries a `position` and nothing else -- no rotation, no scale. `camera` carries fov/near/
//     far. So an authored entity's placement IS a position.
//   * `Renderable::mesh_id` (render_world.h) is an opaque handle with NO registry anywhere. The only
//     mapping in the tree is the hardcoded lit-golden 0 = ground / 1 = blocker in
//     src/render/lit/src/lit_scene.cpp. There is no mesh pipeline to look for.
//
// So this pass draws a BOX PROXY per renderable, tinted with its `Renderable::color`. It honours the
// full render-side Transform (position + rotation quaternion + scale) because that is the snapshot's
// own contract and a package populating it is free to fill all three -- but for anything the ctx:scene
// schema can express today, that degrades exactly to a translation.
//
// SCOPE (e11b): the target lifecycle (viewport_target.h) + this pass + their T1 tests. No shell
// binding (e11e), no picking (e11g), no gizmos or selection outline (e11h), no camera controls
// (e11f), no extract/culling change, no daemon or contract work (e11c).
//
// ⚠ RESOURCE OWNERSHIP IS PER-CALL, AND THE FIRST FRAME LOOP MUST CHANGE THAT. render_viewport_view
// builds its pipeline (which COMPILES the WGSL below), its uniform buffer and one bind group PER
// DRAW on every call, then destroys them all on return -- so a viewport driven at 60 Hz would
// recompile the shader 60 times a second and churn one bind group per grid line per frame. That is
// affordable now only because nothing drives this in a loop yet. WindowCompositor
// (editor/shell/src/compositor.cpp) already has the shape of the fix: hold the pipeline and layout
// as members, grow the uniform buffer monotonically instead of reallocating, and rebuild bind groups
// only when the draw count changes. A stateless free function has nowhere to put that cache, so
// whoever introduces the frame loop owns adding it. Recorded here so it is a KNOWN deferral rather
// than a cost silently inherited.
//
// ⚠ WHY `target_size` IS A SEPARATE PARAMETER. `ITextureView` exposes no extent, and view.h fixes
// the rule ("The framed ASPECT RATIO is never stored on a View. It belongs to the target the view
// is rendered into, so every entry point below takes that target's extent and derives it") -- which
// is why projection_matrix / view_proj / project all take an Extent2D too. Without the extent this
// function cannot build a projection at all. Everything else is defaulted config.
//
// GPU-free of any concrete backend (it drives only rhi.h), so it builds and is unit-tested against
// rendertest::FakeDevice under every toolchain, like the rest of context_render.

#pragma once

#include "context/render/render_world.h"
#include "context/render/rhi.h"
#include "context/render/view.h"

#include <cstdint>

namespace context::render
{

// The ground grid an editor Scene viewport draws under the geometry. Lines lie in the y = 0 plane.
//
// ⚠ A grid LINE is a thin BOX, not a line primitive: the T1 RHI's PrimitiveTopology has exactly one
// value, TriangleList (rhi.h), so there is no line topology to reach for. A box also survives an
// oblique camera, where an infinitely thin ground quad seen edge-on disappears.
struct ViewportGridConfig
{
    // World-space metres between adjacent lines.
    float spacing = 1.0f;
    // Lines each side of the origin, per axis -- so 2 * half_lines + 1 lines run parallel to X and
    // as many parallel to Z.
    std::uint32_t half_lines = 10;
    // Every major_every-th line off the origin is drawn in `major`. 0 is read as 1 (all major).
    std::uint32_t major_every = 10;
    // World-space thickness of a line box.
    float line_width = 0.02f;

    // The palette. Named after the e06 viewport tokens (gridMinor / gridMajor / axisX / axisZ) that
    // the editor theme already carries; binding this to a live theme is e11e/e11h's, not this task's.
    Color minor{0.32, 0.33, 0.36, 1.0};
    Color major{0.48, 0.49, 0.53, 1.0};
    Color axis_x{0.78, 0.26, 0.30, 1.0};
    Color axis_z{0.26, 0.44, 0.78, 1.0};
};

// How many line boxes `grid` draws: 2 * (2 * half_lines + 1).
[[nodiscard]] constexpr std::uint32_t grid_line_count(const ViewportGridConfig& grid)
{
    return 2u * (2u * grid.half_lines + 1u);
}

struct ViewportPassConfig
{
    // The colour the target is cleared to before anything is drawn.
    Color clear{0.09, 0.10, 0.12, 1.0};

    // A Scene viewport draws the grid; a Game viewport (ViewType::game) must not -- D5 gives the game
    // view NO edit-time overlays. Deciding that from the View is e11e's binding job, so it is a
    // caller-set flag here rather than something this pass infers.
    bool draw_grid = true;
    ViewportGridConfig grid{};

    // The world-space edge of the proxy box drawn for a renderable, before the item's own scale.
    float proxy_size = 1.0f;
    // How much the proxy's faces are shaded apart (0 = flat). Without it a box is an untextured
    // silhouette and its orientation is invisible. The grid always draws flat.
    float proxy_shade = 0.35f;

    // The depth attachment, sized to the colour target -- ViewportTargetRegistry::depth_view() is the
    // supplier. nullptr draws with NO depth test, i.e. in submission order, which is honest for a
    // 2D view and wrong for a 3D one.
    ITextureView* depth = nullptr;

    // The colour target's format. RGBA8Unorm for a registry target; BGRA8Unorm when a caller renders
    // straight into a swapchain backbuffer.
    TextureFormat color_format = TextureFormat::RGBA8Unorm;
};

// What one render_viewport_view call actually recorded.
struct ViewportFrameStats
{
    std::uint32_t grid_draws = 0;
    std::uint32_t proxy_draws = 0;
    // Renderables skipped because their Transform was not finite (a NaN position, an infinite scale).
    // Drawing one would put a NaN through the whole matrix chain.
    std::uint32_t skipped_items = 0;
    // Whether a render pass was recorded and submitted at all. False only for a degenerate target.
    bool rendered = false;
};

// The per-draw uniform block. Uploaded verbatim, so it must match `struct Draw` in
// viewport_pass_wgsl() field for field.
struct ViewportDrawUniform
{
    float mvp[16];   // world -> clip, column-major (WGSL mat4x4<f32> order)
    float color[4];  // flat RGBA tint
    float shade[4];  // x = face-shading amount; yzw padding for the WGSL vec4 alignment
};

// The shader-layout invariant, asserted rather than commented: a mismatch would upload silently
// misaligned matrices, which renders as geometry in the wrong place rather than as any error.
static_assert(sizeof(ViewportDrawUniform) == 96u,
              "ViewportDrawUniform must match WGSL `struct Draw { mvp : mat4x4<f32>, color : "
              "vec4<f32>, shade : vec4<f32> }`");

// Every draw in the pass reads its OWN slice of ONE uniform buffer. The stride is WebGPU's
// minUniformBufferOffsetAlignment (256), NOT sizeof(ViewportDrawUniform): a dynamic offset must be
// 256-aligned, so the block is padded up to it.
//
// The alternative -- one buffer per draw, as WindowCompositor::draw_layer does per layer -- is what
// this avoids: a queue write is ordered BEFORE the submitted pass, so several draws in one pass
// sharing one buffer at offset 0 would all read the LAST value written. Distinct offsets solve that
// with a single allocation instead of one per grid line.
inline constexpr std::uint64_t kViewportDrawUniformStride = 256u;

// The proxy/grid box is 36 vertices (12 triangles), generated in the vertex stage from a constant
// corner table -- no vertex buffer, matching every other T1 path in this repo.
inline constexpr std::uint32_t kViewportBoxVertexCount = 36;

// The WGSL both the grid and the proxies draw with (binding 0 = the ViewportDrawUniform block).
[[nodiscard]] const char* viewport_pass_wgsl();

// The pipeline description for that shader. `depth` wires Depth32Float testing + writing (Less),
// which a 3D viewport needs and a depth-attachment-less caller cannot have.
[[nodiscard]] RenderPipelineDesc make_viewport_pipeline_desc(TextureFormat color_format, bool depth);

// Render ONE viewport view of `snapshot` into `target`.
//
// Draw order is fixed and is part of the contract a headless test reads back: every grid line first
// (all the lines parallel to X, from index -half_lines to +half_lines, then all the lines parallel to
// Z in the same order), then one proxy per surviving renderable in snapshot.items order. Each draw
// uploads its ViewportDrawUniform to its own slot before the pass is recorded.
//
// The target is CLEARED even when there is nothing to draw -- a viewport with an empty scene and no
// grid shows the clear colour, not last frame's pixels. A degenerate `target_size` records nothing at
// all and returns `rendered == false`.
ViewportFrameStats render_viewport_view(IDevice& device, const View& view,
                                        const RenderSnapshot& snapshot, ITextureView& target,
                                        Extent2D target_size,
                                        const ViewportPassConfig& config = {});

} // namespace context::render
