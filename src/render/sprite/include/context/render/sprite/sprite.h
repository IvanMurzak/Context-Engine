// Sprite + sorting-layer data types for the 2D path (R-2D-001, L-55).
//
// A Sprite2D is a render-side, float, interpolatable 2D drawable — the 2D analogue of the render
// module's Transform+Renderable (context/render/render_world.h), specialized for the ortho path. It
// is a pure value type (no kernel dependency here) so the sprite CPU logic — sorting, batching, atlas
// lookup — is unit-tested with no GPU and no World. Sorting layers give artists explicit draw-order
// control (the Unity/Godot 2D model): draw order is (sort_layer, order_in_layer), NOT depth.

#pragma once

#include "context/render/sprite/atlas.h"

#include <cstdint>
#include <string>

namespace context::render::sprite
{

// A named sorting layer with an explicit integer order key. Lower `order` draws first (further back);
// higher draws later (on top). Layers are the coarse draw-order control; order_in_layer is the fine
// one within a layer. (Mirrors Unity's SortingLayer / Godot's CanvasItem z-index model.)
struct SortingLayer
{
    std::string name;
    std::int32_t order = 0;
};

// A 2D sprite: an axis-aligned world rectangle (center + size, world units, y-up), a flat tint, an
// atlas/texture id used as the batch key, and its UV rect within that atlas. Draw order is the pair
// (sort_layer, order_in_layer); ties keep input order (a stable sort), so authoring order is the
// final tie-break.
struct Sprite2D
{
    float position[2] = {0.0f, 0.0f}; // world-space center (x,y)
    float size[2] = {1.0f, 1.0f};     // world-space width,height
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    std::int32_t sort_layer = 0;      // coarse draw order (a SortingLayer::order value)
    std::int32_t order_in_layer = 0;  // fine draw order within the layer

    std::uint32_t atlas_id = 0;       // the atlas/texture this sprite samples — the batch key
    UVRect uv;                        // sub-rectangle within the atlas (default = whole texture)
};

} // namespace context::render::sprite
