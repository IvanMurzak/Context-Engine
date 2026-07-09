// CPU unit test: sprite draw-order sorting + batch coalescing (context/render/sprite/batch.h). No GPU.

#include "context/render/sprite/batch.h"

#include "render_test.h"

using namespace context::render::sprite;

namespace
{

Sprite2D make(std::int32_t layer, std::int32_t order, std::uint32_t atlas)
{
    Sprite2D s;
    s.sort_layer = layer;
    s.order_in_layer = order;
    s.atlas_id = atlas;
    return s;
}

void test_sort_by_layer_then_order_then_stable()
{
    // Input (index): 0:(L1,o0) 1:(L0,o5) 2:(L0,o1) 3:(L1,o0) 4:(L0,o1)
    std::vector<Sprite2D> sprites{make(1, 0, 0), make(0, 5, 0), make(0, 1, 0), make(1, 0, 0),
                                  make(0, 1, 0)};
    const std::vector<std::uint32_t> order = sort_draw_order(sprites);
    // Expected draw order: layer 0 first (by order_in_layer, ties stable): idx2(o1), idx4(o1), idx1(o5)
    // then layer 1 (ties stable): idx0, idx3.
    const std::vector<std::uint32_t> expected{2, 4, 1, 0, 3};
    CHECK(order == expected);
}

void test_batches_coalesce_adjacent_same_atlas()
{
    // Draw order all in one layer, atlases: A A B B A  -> 3 batches (A,A | B,B | A), draw order kept.
    std::vector<Sprite2D> sprites{make(0, 0, 7), make(0, 1, 7), make(0, 2, 9),
                                  make(0, 3, 9), make(0, 4, 7)};
    const std::vector<SpriteBatch> batches = build_batches(sprites);
    CHECK(batches.size() == 3);
    CHECK(batches[0].atlas_id == 7 && batches[0].sprite_indices.size() == 2);
    CHECK(batches[1].atlas_id == 9 && batches[1].sprite_indices.size() == 2);
    CHECK(batches[2].atlas_id == 7 && batches[2].sprite_indices.size() == 1);
    // Concatenated batch indices reproduce the full draw order (no sprite dropped or reordered).
    std::vector<std::uint32_t> flat;
    for (const SpriteBatch& b : batches)
    {
        for (std::uint32_t i : b.sprite_indices)
        {
            flat.push_back(i);
        }
    }
    CHECK(flat == sort_draw_order(sprites));
}

void test_batching_never_reorders_across_layers()
{
    // Same atlas across two layers must NOT merge into one batch if a different-atlas sprite sits
    // between them in draw order — draw order (painter's) is authoritative over draw-call merging.
    // Layer 0: atlas 1 ; Layer 1: atlas 2 ; Layer 2: atlas 1. Draw order 1,2,1 -> 3 batches.
    std::vector<Sprite2D> sprites{make(0, 0, 1), make(2, 0, 1), make(1, 0, 2)};
    const std::vector<SpriteBatch> batches = build_batches(sprites);
    CHECK(batches.size() == 3);
    CHECK(batches[0].atlas_id == 1);
    CHECK(batches[1].atlas_id == 2);
    CHECK(batches[2].atlas_id == 1);
}

void test_empty_input()
{
    std::vector<Sprite2D> sprites;
    CHECK(sort_draw_order(sprites).empty());
    CHECK(build_batches(sprites).empty());
}

} // namespace

int main()
{
    test_sort_by_layer_then_order_then_stable();
    test_batches_coalesce_adjacent_same_atlas();
    test_batching_never_reorders_across_layers();
    test_empty_input();
    RENDER_TEST_MAIN_END();
}
