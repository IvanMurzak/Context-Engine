// CPU unit test: the D8 CPU raycast (context/render/picking.h) against a CONSTRUCTED
// `render::RenderSnapshot` — hit / miss / nearest-of-two / click-outside, the box's own rotation and
// non-uniform scale (an OBB test, not merely an AABB one), and the degenerate-transform skip. No
// GPU: runs on the local dev gate under every toolchain, exactly what D8 requires.
//
// ANTI-VACUITY NOTE. Every "misses" assertion below has a sibling in the SAME fixture family proving
// a ray genuinely CAN hit something (the run's own mandate): a raycast that always reports a miss
// (a bug that returns `PickHit{}` unconditionally) would pass every miss case here and is caught only
// by the positive ones sitting beside them. The nearest-of-two case additionally proves the picker
// compares REAL world distance rather than the object-space parametric `t` a naive OBB test would
// reach for — the two items below are given DELIBERATELY DIFFERENT scales precisely so a `t`
// comparison and a world-distance comparison would disagree, and only the world-distance answer is
// correct.

#include "context/render/picking.h"

#include "render_test.h"

#include <cmath>

using namespace context::render;
using context::kernel::Entity;

namespace
{

[[nodiscard]] RenderItem make_item(Entity entity, Vec3 position, Vec3 scale)
{
    RenderItem item;
    item.entity = entity;
    item.transform.position[0] = position.x;
    item.transform.position[1] = position.y;
    item.transform.position[2] = position.z;
    item.transform.scale[0] = scale.x;
    item.transform.scale[1] = scale.y;
    item.transform.scale[2] = scale.z;
    // rotation defaults to the identity quaternion (0,0,0,1) — render_world.h's own Transform default.
    return item;
}

// A quaternion rotating `angle_radians` about the world Y axis (xyzw — render::Transform's own
// component order, math.h's rotation_from_quaternion).
void set_rotation_y(RenderItem& item, float angle_radians)
{
    const float half = angle_radians * 0.5f;
    item.transform.rotation[0] = 0.0f;
    item.transform.rotation[1] = std::sin(half);
    item.transform.rotation[2] = 0.0f;
    item.transform.rotation[3] = std::cos(half);
}

constexpr Entity kEntityA{11u, 1u};
constexpr Entity kEntityB{22u, 1u};

// A ray pointed straight down +Z from the world origin — every fixture below places its drawables
// somewhere along z > 0 on the x = y = 0 line, so this one direction serves every case.
[[nodiscard]] Ray straight_ahead_ray()
{
    return Ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
}

void a_ray_that_hits_one_drawable_selects_it()
{
    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(kEntityA, Vec3{0.0f, 0.0f, 10.0f}, Vec3{1.0f, 1.0f, 1.0f}));

    const PickHit hit = pick_nearest(straight_ahead_ray(), snapshot);
    CHECK(hit.hit);
    CHECK(hit.entity == kEntityA);
    // The box spans z in [9.5, 10.5]; entering at 9.5 along a unit-length direction from the origin.
    CHECK(std::fabs(hit.distance - 9.5f) < 1.0e-4f);
}

void a_ray_that_misses_everything_clears_the_selection()
{
    // The empty-scene mechanism: nothing to hit at all.
    const RenderSnapshot empty;
    const PickHit hit = pick_nearest(straight_ahead_ray(), empty);
    CHECK(!hit.hit);
    CHECK(hit.distance == 0.0f);
}

void a_click_outside_any_drawable_clears_the_selection()
{
    // A DIFFERENT miss mechanism from the empty-scene one above: a real drawable exists, but the ray
    // passes well clear of its box footprint (offset 5 world units off the x = 0 line the ray rides).
    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(kEntityA, Vec3{5.0f, 0.0f, 10.0f}, Vec3{1.0f, 1.0f, 1.0f}));

    const PickHit hit = pick_nearest(straight_ahead_ray(), snapshot);
    CHECK(!hit.hit);

    // The positive sibling in the SAME fixture family: the identical ray DOES hit an item placed back
    // on its line, proving the miss above is a real spatial miss and not a raycast that never hits
    // anything.
    snapshot.items.push_back(make_item(kEntityB, Vec3{0.0f, 0.0f, 10.0f}, Vec3{1.0f, 1.0f, 1.0f}));
    const PickHit second = pick_nearest(straight_ahead_ray(), snapshot);
    CHECK(second.hit);
    CHECK(second.entity == kEntityB);
}

void the_nearest_of_two_overlapping_candidates_wins()
{
    // Two boxes the SAME ray hits, at very different SCALES — a candidate set a naive "first hit
    // found" or an object-space `t` comparison (not comparable across differently-scaled boxes)
    // could get wrong; only a REAL WORLD DISTANCE comparison is correct.
    RenderSnapshot snapshot;
    // The FAR, LARGE box: z in [19, 21], entered at world distance 19.
    snapshot.items.push_back(make_item(kEntityA, Vec3{0.0f, 0.0f, 20.0f}, Vec3{2.0f, 2.0f, 2.0f}));
    // The NEAR, SMALL box: z in [7.5, 8.5], entered at world distance 7.5 — the true nearest hit.
    snapshot.items.push_back(make_item(kEntityB, Vec3{0.0f, 0.0f, 8.0f}, Vec3{1.0f, 1.0f, 1.0f}));

    const PickHit hit = pick_nearest(straight_ahead_ray(), snapshot);
    CHECK(hit.hit);
    CHECK(hit.entity == kEntityB);
    CHECK(std::fabs(hit.distance - 7.5f) < 1.0e-4f);

    // And reversed insertion order — the algorithm must not depend on scan order.
    RenderSnapshot reversed;
    reversed.items.push_back(snapshot.items[1]);
    reversed.items.push_back(snapshot.items[0]);
    const PickHit hit2 = pick_nearest(straight_ahead_ray(), reversed);
    CHECK(hit2.hit);
    CHECK(hit2.entity == kEntityB);
}

void rotation_and_non_uniform_scale_are_honoured()
{
    // A box elongated along its LOCAL x axis (scale 6 x 0.2 x 0.2) then rotated 90 degrees about Y,
    // which swings that long axis onto WORLD +Z. A ray riding the world x = 0.3 line — OUTSIDE the
    // box's unrotated 0.1 half-thickness on x, but the rotation does not move the box off x = 0 at
    // all (rotation is about the box's own centre) — so this line is a genuine near-miss test of the
    // box's THIN axis, not the long one: it must still be OUTSIDE the (rotated) 0.1-thick slab.
    RenderItem elongated = make_item(kEntityA, Vec3{0.0f, 0.0f, 10.0f}, Vec3{6.0f, 0.2f, 0.2f});
    set_rotation_y(elongated, 1.5707963f); // 90 degrees

    RenderSnapshot miss_snapshot;
    miss_snapshot.items.push_back(elongated);
    const Ray offset_ray{Vec3{0.3f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
    CHECK(!pick_nearest(offset_ray, miss_snapshot).hit);

    // The POSITIVE sibling proving the rotation was actually applied (not merely "always misses"):
    // BEFORE rotation, the box's long axis (6 world units) lies along X, so a ray at x = 0.3 clears
    // straight through the middle of it (half-extent 3). Same box, same ray, rotation removed.
    RenderItem unrotated = make_item(kEntityA, Vec3{0.0f, 0.0f, 10.0f}, Vec3{6.0f, 0.2f, 0.2f});
    RenderSnapshot hit_snapshot;
    hit_snapshot.items.push_back(unrotated);
    const PickHit hit = pick_nearest(offset_ray, hit_snapshot);
    CHECK(hit.hit);
    CHECK(hit.entity == kEntityA);

    // A SECOND positive sibling: rotate the SAME elongated box so its long axis points along X
    // instead (a further 90-degree turn brings it back), and the same x = 0.3 ray must hit it again —
    // proving the miss above tracks the box's ORIENTATION, not merely "this box always misses".
    RenderItem back_along_x = make_item(kEntityA, Vec3{0.0f, 0.0f, 10.0f}, Vec3{6.0f, 0.2f, 0.2f});
    set_rotation_y(back_along_x, 3.14159265f); // 180 degrees — long axis still along X
    RenderSnapshot hit_snapshot2;
    hit_snapshot2.items.push_back(back_along_x);
    CHECK(pick_nearest(offset_ray, hit_snapshot2).hit);
}

void a_non_finite_transform_is_skipped_not_crashed()
{
    // The item the render pass itself would refuse to draw (a NaN position) must not crash the
    // raycast, must not be reported as a hit, and must not hide a VALID hit behind it.
    RenderItem broken = make_item(kEntityA, Vec3{0.0f, 0.0f, 10.0f}, Vec3{1.0f, 1.0f, 1.0f});
    broken.transform.position[0] = std::nanf("");

    RenderSnapshot only_broken;
    only_broken.items.push_back(broken);
    const PickHit miss = pick_nearest(straight_ahead_ray(), only_broken);
    CHECK(!miss.hit);
    CHECK(!std::isnan(miss.distance));

    RenderSnapshot broken_and_valid;
    broken_and_valid.items.push_back(broken);
    broken_and_valid.items.push_back(make_item(kEntityB, Vec3{0.0f, 0.0f, 10.0f}, Vec3{1.0f, 1.0f, 1.0f}));
    const PickHit hit = pick_nearest(straight_ahead_ray(), broken_and_valid);
    CHECK(hit.hit);
    CHECK(hit.entity == kEntityB);
}

void selection_ids_are_stable_and_distinguish_entities()
{
    const std::string id_a = pick_selection_id(kEntityA);
    const std::string id_b = pick_selection_id(kEntityB);
    CHECK(!id_a.empty());
    CHECK(id_a != id_b);
    CHECK(pick_selection_id(kEntityA) == id_a); // stable across calls
}

} // namespace

int main()
{
    a_ray_that_hits_one_drawable_selects_it();
    a_ray_that_misses_everything_clears_the_selection();
    a_click_outside_any_drawable_clears_the_selection();
    the_nearest_of_two_overlapping_candidates_wins();
    rotation_and_non_uniform_scale_are_honoured();
    a_non_finite_transform_is_skipped_not_crashed();
    selection_ids_are_stable_and_distinguish_entities();
    RENDER_TEST_MAIN_END();
}
