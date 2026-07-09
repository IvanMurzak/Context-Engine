// The render world — the double-buffered snapshot of render-relevant simulation state (L-39).
//
// L-39: an extract step copies render-relevant state out of the sim World into a separate,
// double-buffered render world; the render side reads ONLY that snapshot, so sim and render never
// tear and neither blocks the other. Headless = the extract step never runs (R-HEAD-002/003).
//
// Render defines its OWN render-relevant components (float, interpolatable) — distinct from the
// deterministic integer sim components in src/runtime/session/. A transform/renderable package
// populates them on entities; the extract reads them. The kernel does NOT depend on this header —
// render depends on the kernel, never the reverse.

#pragma once

#include "context/kernel/entity.h"

#include <cstdint>
#include <vector>

namespace context::render
{

// A render-relevant transform: float, world-space, interpolatable between fixed sim ticks (the
// render-side interpolation contract is R-SIM-002, a later wave). Rotation is a quaternion (xyzw).
struct Transform
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

// A render-relevant tag: an entity carrying Renderable (alongside a Transform) is drawn. mesh_id is
// an opaque handle into a mesh/material registry (a later wave); color is a flat tint for now.
struct Renderable
{
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    std::uint32_t mesh_id = 0;
};

// One extracted drawable: the entity it came from plus a copy of its render state.
struct RenderItem
{
    kernel::Entity entity;
    Transform transform;
    Renderable renderable;
};

// A complete render-side snapshot of one sim tick's drawables. A plain value type: copied/swapped by
// the double buffer, read by the render side with no reference back into the sim World.
struct RenderSnapshot
{
    std::uint64_t sim_tick = 0;
    std::vector<RenderItem> items;

    void clear()
    {
        sim_tick = 0;
        items.clear();
    }
};

// The L-39 double buffer: the sim/extract side writes the BACK snapshot; the render side reads the
// FRONT snapshot; swap() flips them at a tick boundary. Because the two never alias, the render side
// always reads a complete, consistent snapshot while the next one is being extracted — no tearing.
// Two buffers also give render-side interpolation its two endpoints (front = previous, back = next).
class RenderDoubleBuffer
{
public:
    // The snapshot the extract step writes into (the next frame).
    [[nodiscard]] RenderSnapshot& back() { return buffers_[back_index()]; }

    // The snapshot the render side reads (the current frame).
    [[nodiscard]] const RenderSnapshot& front() const { return buffers_[front_]; }

    // Publish the freshly-extracted back buffer as the new front. The old front becomes the next
    // back and is cleared before it is written again.
    void swap()
    {
        front_ = back_index();
        buffers_[back_index()].clear();
    }

private:
    [[nodiscard]] std::size_t back_index() const { return front_ ^ 1u; }

    RenderSnapshot buffers_[2];
    std::size_t front_ = 0;
};

} // namespace context::render
