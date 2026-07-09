// Sprite batching + sort-layer ordering — see context/render/sprite/batch.h.

#include "context/render/sprite/batch.h"

#include <algorithm>
#include <cstdint>

namespace context::render::sprite
{

std::vector<std::uint32_t> sort_draw_order(const std::vector<Sprite2D>& sprites)
{
    std::vector<std::uint32_t> order(sprites.size());
    for (std::uint32_t i = 0; i < sprites.size(); ++i)
    {
        order[i] = i;
    }
    // stable_sort so equal (sort_layer, order_in_layer) keys keep original authoring order — the
    // documented final tie-break. Painter's order: lower keys drawn first (further back).
    std::stable_sort(order.begin(), order.end(),
                     [&sprites](std::uint32_t a, std::uint32_t b)
                     {
                         const Sprite2D& sa = sprites[a];
                         const Sprite2D& sb = sprites[b];
                         if (sa.sort_layer != sb.sort_layer)
                         {
                             return sa.sort_layer < sb.sort_layer;
                         }
                         return sa.order_in_layer < sb.order_in_layer;
                     });
    return order;
}

std::vector<SpriteBatch> build_batches(const std::vector<Sprite2D>& sprites)
{
    std::vector<SpriteBatch> batches;
    const std::vector<std::uint32_t> order = sort_draw_order(sprites);

    for (std::uint32_t index : order)
    {
        const std::uint32_t atlas = sprites[index].atlas_id;
        // Extend the current batch only if it shares the atlas AND is adjacent in draw order (which it
        // always is here — we walk draw order). A change of atlas breaks the run into a new batch, so
        // draw order is never reordered for the sake of merging.
        if (!batches.empty() && batches.back().atlas_id == atlas)
        {
            batches.back().sprite_indices.push_back(index);
        }
        else
        {
            SpriteBatch batch;
            batch.atlas_id = atlas;
            batch.sprite_indices.push_back(index);
            batches.push_back(std::move(batch));
        }
    }
    return batches;
}

} // namespace context::render::sprite
