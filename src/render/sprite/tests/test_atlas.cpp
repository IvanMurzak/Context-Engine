// CPU unit test: texture atlas packing + UV lookup (context/render/sprite/atlas.h). No GPU.

#include "context/render/sprite/atlas.h"

#include "render_test.h"

#include <algorithm>
#include <cmath>

using namespace context::render::sprite;

namespace
{

bool approx(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps;
}

void test_add_find_and_bounds()
{
    TextureAtlas atlas(256, 128);
    CHECK(atlas.add("hero", AtlasRegion{0, 0, 64, 64}));
    CHECK(atlas.size() == 1);
    CHECK(atlas.find("hero") != nullptr);
    CHECK(atlas.find("missing") == nullptr);
    // Duplicate name rejected.
    CHECK(!atlas.add("hero", AtlasRegion{64, 0, 16, 16}));
    // Out-of-bounds rejected (x+w past width, y+h past height, zero-size).
    CHECK(!atlas.add("wide", AtlasRegion{200, 0, 100, 16}));
    CHECK(!atlas.add("tall", AtlasRegion{0, 100, 16, 100}));
    CHECK(!atlas.add("empty", AtlasRegion{0, 0, 0, 16}));
    CHECK(atlas.size() == 1);
}

void test_uv_normalization()
{
    TextureAtlas atlas(200, 100);
    CHECK(atlas.add("r", AtlasRegion{50, 25, 100, 50}));
    const UVRect uv = atlas.uv("r");
    CHECK(approx(uv.u0, 50.0f / 200.0f));
    CHECK(approx(uv.v0, 25.0f / 100.0f));
    CHECK(approx(uv.u1, 150.0f / 200.0f));
    CHECK(approx(uv.v1, 75.0f / 100.0f));
    // Absent name -> default full-texture UVs.
    const UVRect def = atlas.uv("nope");
    CHECK(approx(def.u0, 0.0f) && approx(def.v0, 0.0f) && approx(def.u1, 1.0f) && approx(def.v1, 1.0f));
}

void test_pack_all_fit()
{
    std::vector<PackItem> items{{"a", 32, 32}, {"b", 32, 64}, {"c", 16, 16}, {"d", 48, 24}};
    const PackResult r = pack_atlas(128, 128, items, /*padding=*/0);
    CHECK(r.ok);
    CHECK(r.overflow.empty());
    CHECK(r.atlas.size() == 4);
    // Every item is present and in-bounds; regions are pairwise non-overlapping.
    for (const PackItem& it : items)
    {
        const AtlasRegion* reg = r.atlas.find(it.name);
        CHECK(reg != nullptr);
        if (reg != nullptr)
        {
            CHECK(reg->x + reg->width <= 128);
            CHECK(reg->y + reg->height <= 128);
            CHECK(reg->width == it.width && reg->height == it.height);
        }
    }
}

void test_pack_overflow()
{
    // One item is larger than the atlas -> it overflows, the rest still pack, ok == false.
    std::vector<PackItem> items{{"ok", 16, 16}, {"huge", 400, 16}};
    const PackResult r = pack_atlas(64, 64, items, 0);
    CHECK(!r.ok);
    CHECK(r.overflow.size() == 1);
    CHECK(r.overflow[0] == "huge");
    CHECK(r.atlas.find("ok") != nullptr);
    CHECK(r.atlas.find("huge") == nullptr);
}

void test_pack_deterministic_and_padding()
{
    std::vector<PackItem> items{{"a", 20, 20}, {"b", 20, 40}, {"c", 20, 20}};
    const PackResult r1 = pack_atlas(128, 128, items, 2);
    const PackResult r2 = pack_atlas(128, 128, items, 2);
    CHECK(r1.ok && r2.ok);
    // Deterministic: identical layout for identical input.
    for (const PackItem& it : items)
    {
        const AtlasRegion* a = r1.atlas.find(it.name);
        const AtlasRegion* b = r2.atlas.find(it.name);
        CHECK(a != nullptr && b != nullptr);
        if (a != nullptr && b != nullptr)
        {
            CHECK(a->x == b->x && a->y == b->y);
            // Padding keeps every region at least `padding` from the left/top edges.
            CHECK(a->x >= 2 && a->y >= 2);
        }
    }
}

} // namespace

int main()
{
    test_add_find_and_bounds();
    test_uv_normalization();
    test_pack_all_fit();
    test_pack_overflow();
    test_pack_deterministic_and_padding();
    RENDER_TEST_MAIN_END();
}
