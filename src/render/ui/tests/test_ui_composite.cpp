// Composite math (context/render/ui/composite.h): the transform + opacity fold the UI backend applies
// at composite time (R-UI-005 composited_transforms) — the DoD "composite math" fake-backend coverage.

#include "context/render/ui/composite.h"

#include "render_test.h"

using namespace context::render::ui;
using namespace context::packages::ui;

namespace
{

bool rect_eq(const Rect& a, const Rect& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

void test_apply_transform_identity_translate_scale()
{
    const Rect base{10, 10, 50, 30};

    // Identity transform ⇒ unchanged bounds.
    CHECK(rect_eq(apply_transform(base, Transform{}), base));

    // Translate shifts the centre (and hence the rect) by the translate vector, size unchanged.
    Transform t;
    t.translate = {5.0f, -5.0f};
    CHECK(rect_eq(apply_transform(base, t), Rect{15, 5, 50, 30}));

    // Scale about the CENTRE: (10,10,50,30) centre (35,25), x2 -> (100x60) centred on (35,25).
    Transform s;
    s.scale = {2.0f, 2.0f};
    CHECK(rect_eq(apply_transform(base, s), Rect{-15, -5, 100, 60}));

    // Combined: scale 1.25 + translate — the badge case (200,20,32,32) -> (192,20,40,40).
    Transform badge;
    badge.scale = {1.25f, 1.25f};
    badge.translate = {-4.0f, 4.0f};
    CHECK(rect_eq(apply_transform(Rect{200, 20, 32, 32}, badge), Rect{192, 20, 40, 40}));

    // Non-positive scale collapses to empty (nothing to draw).
    Transform zero;
    zero.scale = {0.0f, 1.0f};
    CHECK(apply_transform(base, zero).empty());
}

void test_effective_opacity_composes_and_clamps()
{
    CHECK(effective_opacity(1.0f, 1.0f) == 1.0f);
    CHECK(effective_opacity(0.5f, 0.5f) == 0.25f);
    CHECK(effective_opacity(0.5f, 0.0f) == 0.0f);
    // Clamped into [0,1] (a >1 style value never over-brightens the product).
    CHECK(effective_opacity(2.0f, 1.0f) == 1.0f);
    CHECK(effective_opacity(-1.0f, 1.0f) == 0.0f);
}

void test_blend_over_folds_opacity()
{
    const Color black{0, 0, 0, 255};
    const Color white{255, 255, 255, 255};

    // Full opacity ⇒ source wins; zero ⇒ backdrop wins; always opaque out.
    CHECK(blend_over(black, white, 1.0f) == white);
    CHECK(blend_over(black, white, 0.0f) == black);
    // Half opacity over black ⇒ mid grey (255*0.5 rounded). Extra parens: the braced commas would
    // otherwise be split into multiple CHECK() macro arguments.
    CHECK((blend_over(black, white, 0.5f) == Color{128, 128, 128, 255}));
    // A fully-transparent source contributes nothing regardless of opacity.
    CHECK(blend_over(black, Color{255, 0, 0, 0}, 1.0f) == black);
}

} // namespace

int main()
{
    test_apply_transform_identity_translate_scale();
    test_effective_opacity_composes_and_clamps();
    test_blend_over_folds_opacity();
    RENDER_TEST_MAIN_END();
}
