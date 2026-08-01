// CPU unit test: the D5 Camera/View abstraction (context/render/view.h) — the view/projection
// composition, the project <-> unproject round trip in BOTH modes, pick_ray, and the
// degenerate-input finiteness contract. No GPU — runs on the local dev gate under every toolchain.
//
// ANTI-VACUITY NOTE, and it is the load-bearing one for this file. A
// `unproject(project(p)) == p` round trip is INVARIANT to any INVERTIBLE error in the
// view-projection: transpose the view matrix, halve the field of view, swap the aspect ratio — the
// round trip still closes, because unproject inverts whatever project did. So the round-trip cases
// below prove exactly one thing, that inverse() inverts, and they are deliberately NOT the only
// cases here. The projection itself is pinned by INDEPENDENT positive assertions that name expected
// values: test_view_matrix_is_the_inverse_of_the_camera_placement (the camera lands at the view
// origin, its own forward/right/up axes land on -Z/+X/+Y), test_projection_pins_frustum_edges (a
// point at the frustum edge lands at NDC +-1), and test_two_d_mode_is_orthographic (a depth change
// moves nothing laterally). Each of those fails for a mutation the round trip cannot see.
//
// Fixture discipline: the camera is OFF-AXIS (a 37-degree rotation about a non-axis-aligned axis)
// at a position off-centre in all three world axes, the target is 16:9 (never 1:1), the field of
// view is 50 degrees (never 90, where tan(fov/2) == 1 hides a dropped factor), near/far are
// 0.25/120 (never 0/1), and every probe point is off-centre in all three CAMERA axes as well — so
// a transposed, mis-scaled, axis-swapped or half-implemented matrix cannot coincidentally agree.

#include "context/render/view.h"

#include "render_test.h"

#include <cmath>
#include <cstdint>

using namespace context::render;

namespace
{

// Tolerances, all float32 (~7 significant decimal digits). A projected coordinate costs ~8
// multiply-adds (~1e-6 relative); the round trip adds a 4x4 cofactor inverse whose terms span
// far/near = 480, which is where error actually accumulates. MEASURED worst cases over exactly the
// fixtures below, on this host: 9.5e-7 for the direct view/projection asserts, 3.6e-5 ABSOLUTE for
// the 3D round trip (on world coordinates of magnitude ~21), 2.9e-6 for the 2D round trip, 1.1e-5
// for the pick-ray incidence distance and its direction normalization, 6.5e-6 for the pick-ray
// reprojection. Each constant sits 8x-21x above its measurement — loose enough to survive another
// toolchain's rounding, and orders of magnitude BELOW what any wrong matrix produces here (the
// smallest planted defect in this file's PLANT table moves a projected coordinate by ~0.06 and
// pushes the round trip off by ~1).
constexpr float kEps = 2.0e-5f;
constexpr float kRoundTripEps = 3.0e-4f;
constexpr float kRayEps = 1.0e-4f;
constexpr float kReprojectEps = 1.0e-4f;

constexpr float kFovY = 0.87266463f; // 50 degrees
constexpr float kNear = 0.25f;
constexpr float kFar = 120.0f;
constexpr std::uint32_t kWidth = 1920u;
constexpr std::uint32_t kHeight = 1080u;

bool near_f(float a, float b, float eps = kEps)
{
    return std::fabs(a - b) <= eps;
}

bool finite3(Vec3 v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite_mat(const Mat4& m)
{
    for (const float v : m.m)
    {
        if (!std::isfinite(v))
        {
            return false;
        }
    }
    return true;
}

Extent2D target()
{
    return Extent2D{kWidth, kHeight};
}

// The off-axis camera orientation: 37 degrees about normalize(0.3, 0.8, -0.5). Built here from
// axis+angle rather than hardcoded so the intent is legible, and applied through the SAME quaternion
// field a scene camera authors.
struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quat fixture_orientation()
{
    const Vec3 axis = normalize(Vec3{0.3f, 0.8f, -0.5f});
    const float angle = 0.64577182f; // 37 degrees
    const float s = std::sin(0.5f * angle);
    return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(0.5f * angle)};
}

View fixture_view_3d()
{
    const Quat q = fixture_orientation();
    View v;
    v.transform.position[0] = 7.5f;
    v.transform.position[1] = 4.25f;
    v.transform.position[2] = -3.75f;
    v.transform.rotation[0] = q.x;
    v.transform.rotation[1] = q.y;
    v.transform.rotation[2] = q.z;
    v.transform.rotation[3] = q.w;
    v.projection.fov_y_radians = kFovY;
    v.projection.near_z = kNear;
    v.projection.far_z = kFar;
    v.mode = ViewMode::three_d;
    v.type = ViewType::scene;
    v.viewport_id = 3u;
    return v;
}

View fixture_view_2d()
{
    View v = fixture_view_3d();
    v.mode = ViewMode::two_d;
    v.projection.ortho_half_height = 3.25f;
    v.projection.near_z = 0.0f;
    v.projection.far_z = 50.0f;
    v.type = ViewType::game;
    v.viewport_id = 4u;
    return v;
}

// The camera's own world-space basis, derived from the SAME quaternion the View carries. Used to
// place probe points relative to the camera without re-deriving the view matrix under test.
Vec3 camera_axis(const View& v, Vec3 local)
{
    const Mat4 r = rotation_from_quaternion(v.transform.rotation[0], v.transform.rotation[1],
                                            v.transform.rotation[2], v.transform.rotation[3]);
    return transform_point(r, local);
}

Vec3 camera_position(const View& v)
{
    return Vec3{v.transform.position[0], v.transform.position[1], v.transform.position[2]};
}

// A world point at `depth` along the camera forward, offset `rx` right and `ry` up.
Vec3 point_in_front(const View& v, float depth, float rx, float ry)
{
    const Vec3 forward = camera_axis(v, Vec3{0.0f, 0.0f, -1.0f});
    const Vec3 right = camera_axis(v, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 up = camera_axis(v, Vec3{0.0f, 1.0f, 0.0f});
    Vec3 p = add(camera_position(v), scale(forward, depth));
    p = add(p, scale(right, rx));
    return add(p, scale(up, ry));
}

// ---------------------------------------------------------------------------------------------
// The type itself (DoD: transform, projection params, mode 2D|3D, type Scene|Game, viewport id)
// ---------------------------------------------------------------------------------------------

void test_view_defaults_and_fields()
{
    const View fresh;
    CHECK(fresh.mode == ViewMode::three_d);      // VIEW default mode is 3D
    CHECK(fresh.type == ViewType::scene);        // VIEW default type is Scene
    CHECK(fresh.viewport_id == 0u);              // VIEW default viewport id is unassigned
    CHECK(near_f(fresh.transform.scale[0], 1.0f));
    CHECK(near_f(fresh.transform.rotation[3], 1.0f)); // identity quaternion

    const View scene = fixture_view_3d();
    const View game = fixture_view_2d();
    CHECK(scene.type == ViewType::scene && game.type == ViewType::game);
    CHECK(scene.mode == ViewMode::three_d && game.mode == ViewMode::two_d);
    CHECK(scene.viewport_id == 3u && game.viewport_id == 4u); // VIEW carries its viewport id
}

void test_aspect_ratio_and_its_degenerate_guard()
{
    CHECK(near_f(aspect_ratio(target()), 16.0f / 9.0f)); // ASPECT is width over height
    CHECK(near_f(aspect_ratio(Extent2D{0u, 0u}), 1.0f)); // ASPECT of a zero extent is 1
    CHECK(near_f(aspect_ratio(Extent2D{0u, kHeight}), 1.0f));
    CHECK(near_f(aspect_ratio(Extent2D{kWidth, 0u}), 1.0f));
}

// ---------------------------------------------------------------------------------------------
// The view matrix — pinned independently of any round trip
// ---------------------------------------------------------------------------------------------

void test_view_matrix_is_the_inverse_of_the_camera_placement()
{
    const View v = fixture_view_3d();
    const Mat4 view = view_matrix(v);

    // The camera itself sits at the view-space origin.
    const Vec3 eye = transform_point(view, camera_position(v));
    CHECK(near_f(eye.x, 0.0f) && near_f(eye.y, 0.0f) && near_f(eye.z, 0.0f));

    // Its own axes land on the view basis: forward -> -Z, right -> +X, up -> +Y. A view matrix
    // built from R instead of R^T (the mutation a round trip cannot see) fails all three.
    const float d = 2.5f;
    const Vec3 ahead = transform_point(view, point_in_front(v, d, 0.0f, 0.0f));
    CHECK(near_f(ahead.x, 0.0f) && near_f(ahead.y, 0.0f));
    CHECK(near_f(ahead.z, -d)); // VIEW MATRIX camera forward maps to view -Z

    const Vec3 sideways = transform_point(view, point_in_front(v, 0.0f, d, 0.0f));
    CHECK(near_f(sideways.x, d) && near_f(sideways.y, 0.0f) && near_f(sideways.z, 0.0f));

    const Vec3 above = transform_point(view, point_in_front(v, 0.0f, 0.0f, d));
    CHECK(near_f(above.y, d)); // VIEW MATRIX camera up maps to view +Y
    CHECK(near_f(above.x, 0.0f) && near_f(above.z, 0.0f));
}

void test_projection_pins_frustum_edges()
{
    const View v = fixture_view_3d();
    const float tan_half = std::tan(0.5f * kFovY);
    const float aspect = 16.0f / 9.0f;
    const float depth = 6.0f;

    // Straight ahead -> the centre of the image, at a depth strictly inside the frustum.
    const Vec3 centre = project(v, target(), point_in_front(v, depth, 0.0f, 0.0f));
    CHECK(near_f(centre.x, 0.0f) && near_f(centre.y, 0.0f));
    CHECK(centre.z > 0.0f && centre.z < 1.0f); // PROJECT in-frustum depth stays in [0,1]

    // At the top edge of the frustum -> NDC y +1; at the right edge -> NDC x +1. These pin the
    // 1/tan(fov/2) factor and the aspect divide THROUGH the view, not just in the raw matrix.
    const Vec3 top = project(v, target(), point_in_front(v, depth, 0.0f, depth * tan_half));
    CHECK(near_f(top.y, 1.0f)); // PROJECT frustum top edge -> NDC y +1
    CHECK(near_f(top.x, 0.0f));

    const Vec3 right =
        project(v, target(), point_in_front(v, depth, depth * tan_half * aspect, 0.0f));
    CHECK(near_f(right.x, 1.0f)); // PROJECT frustum right edge -> NDC x +1
    CHECK(near_f(right.y, 0.0f));

    // Feeding the VERTICAL half-extent horizontally must land at 1/aspect, never 1 and never
    // aspect — an aspect divide applied upside-down fails here.
    const Vec3 narrow = project(v, target(), point_in_front(v, depth, depth * tan_half, 0.0f));
    CHECK(near_f(narrow.x, 1.0f / aspect)); // PROJECT aspect is applied the right way up
}

// ---------------------------------------------------------------------------------------------
// The round trip the DoD names, in BOTH modes
// ---------------------------------------------------------------------------------------------

void test_round_trip_3d()
{
    const View v = fixture_view_3d();
    // Off-centre in all three CAMERA axes (17 ahead, 4.5 right, 2.25 down) and, because the camera
    // is off-axis, in all three WORLD axes too — asserted below rather than assumed.
    const Vec3 p = point_in_front(v, 17.0f, 4.5f, -2.25f);
    CHECK(std::fabs(p.x) > 0.5f && std::fabs(p.y) > 0.5f && std::fabs(p.z) > 0.5f);

    const Vec3 ndc = project(v, target(), p);
    // The probe really is off-centre in the IMAGE too: a point at the image centre would make the
    // x/y halves of this round trip vacuous.
    CHECK(std::fabs(ndc.x) > 0.05f && std::fabs(ndc.y) > 0.05f);
    CHECK(ndc.z > 0.0f && ndc.z < 1.0f);

    const Vec3 back = unproject(v, target(), ndc);
    CHECK(near_f(back.x, p.x, kRoundTripEps)); // ROUND TRIP 3D x
    CHECK(near_f(back.y, p.y, kRoundTripEps)); // ROUND TRIP 3D y
    CHECK(near_f(back.z, p.z, kRoundTripEps)); // ROUND TRIP 3D z
}

void test_round_trip_2d()
{
    const View v = fixture_view_2d();
    const Vec3 p = point_in_front(v, 12.0f, 2.75f, -1.5f);
    const Vec3 ndc = project(v, target(), p);
    CHECK(std::fabs(ndc.x) > 0.05f && std::fabs(ndc.y) > 0.05f);
    CHECK(ndc.z > 0.0f && ndc.z < 1.0f);

    const Vec3 back = unproject(v, target(), ndc);
    CHECK(near_f(back.x, p.x, kRoundTripEps)); // ROUND TRIP 2D x
    CHECK(near_f(back.y, p.y, kRoundTripEps)); // ROUND TRIP 2D y
    CHECK(near_f(back.z, p.z, kRoundTripEps)); // ROUND TRIP 2D z
}

void test_two_d_mode_is_orthographic()
{
    // Two points on the same camera-space ray offset, at very different depths. Under an
    // ORTHOGRAPHIC projection they share an image position; under a perspective one they do not.
    // This is what proves ViewMode is actually READ, which no round trip can show.
    const View flat = fixture_view_2d();
    const Vec3 nearer = point_in_front(flat, 8.0f, 1.75f, 0.5f);
    const Vec3 farther = point_in_front(flat, 30.0f, 1.75f, 0.5f);

    const Vec3 a = project(flat, target(), nearer);
    const Vec3 b = project(flat, target(), farther);
    CHECK(near_f(a.x, b.x)); // ORTHO depth does not move a point horizontally
    CHECK(near_f(a.y, b.y)); // ORTHO depth does not move a point vertically
    CHECK(std::fabs(a.z - b.z) > 0.1f); // ...and the two really are at different depths

    // The SAME pair through the 3D view separates, because perspective divides by depth.
    const View solid = fixture_view_3d();
    const Vec3 pa = project(solid, target(), nearer);
    const Vec3 pb = project(solid, target(), farther);
    CHECK(std::fabs(pa.x - pb.x) > 0.1f); // PERSPECTIVE depth DOES move a point horizontally
}

// ---------------------------------------------------------------------------------------------
// pick_ray
// ---------------------------------------------------------------------------------------------

void test_pick_ray_passes_through_the_picked_point()
{
    const View v = fixture_view_3d();
    // An off-centre pixel in both axes, on no special row or column.
    const std::int32_t px = 1417;
    const std::int32_t py = 322;
    // The documented region-pixel convention, restated here independently of the implementation:
    // physical, region-relative, TOP-LEFT origin, y-DOWN, sampled at the pixel CENTRE.
    const float ndc_x = 2.0f * ((static_cast<float>(px) + 0.5f) / static_cast<float>(kWidth)) - 1.0f;
    const float ndc_y =
        1.0f - 2.0f * ((static_cast<float>(py) + 0.5f) / static_cast<float>(kHeight));
    CHECK(ndc_x > 0.4f && ndc_y > 0.3f); // the chosen pixel is off-centre in BOTH axes

    // A world point on that pixel's line of sight, well inside the frustum.
    const Vec3 picked = unproject(v, target(), Vec3{ndc_x, ndc_y, 0.97f});
    // Cross-check through the FORWARD direction, which test_projection_pins_frustum_edges pins
    // independently: this really is the world point that pixel shows.
    const Vec3 reprojected = project(v, target(), picked);
    // PICK RAY probe point reprojects to its own pixel
    CHECK(near_f(reprojected.x, ndc_x, kReprojectEps));
    CHECK(near_f(reprojected.y, ndc_y, kReprojectEps));

    const Ray ray = pick_ray(v, RegionPoint{px, py}, target());
    CHECK(near_f(length(ray.direction), 1.0f)); // PICK RAY direction is normalized

    const Vec3 to_point = sub(picked, ray.origin);
    const float along = dot(to_point, ray.direction);
    CHECK(along > 0.0f); // PICK RAY the picked point is IN FRONT of the ray origin
    const float off_axis = length(sub(to_point, scale(ray.direction, along)));
    CHECK(off_axis < kRayEps); // PICK RAY passes THROUGH the picked point
}

void test_pick_ray_flips_the_pixel_y_axis()
{
    // Region pixels count DOWN from the top; NDC counts UP. A ray through an upper pixel must
    // therefore point further along the camera's world UP than one through a lower pixel. Drop or
    // duplicate the flip and this inequality reverses.
    const View v = fixture_view_3d();
    const Vec3 up = camera_axis(v, Vec3{0.0f, 1.0f, 0.0f});
    const Ray upper = pick_ray(v, RegionPoint{960, 100}, target());
    const Ray lower = pick_ray(v, RegionPoint{960, 980}, target());
    const float upper_component = dot(upper.direction, up);
    const float lower_component = dot(lower.direction, up);
    CHECK(upper_component > 0.2f);  // PICK RAY an upper pixel looks UP the camera's up axis
    CHECK(lower_component < -0.2f); // PICK RAY a lower pixel looks DOWN it
}

void test_pick_ray_is_parallel_in_two_d_and_divergent_in_three_d()
{
    // The orthographic hallmark: every pick ray shares a direction and differs in ORIGIN. The
    // perspective one is the mirror image. Together they show pick_ray reads ViewMode too.
    const View flat = fixture_view_2d();
    const Ray a = pick_ray(flat, RegionPoint{200, 300}, target());
    const Ray b = pick_ray(flat, RegionPoint{1700, 800}, target());
    CHECK(near_f(a.direction.x, b.direction.x) && near_f(a.direction.y, b.direction.y) &&
          near_f(a.direction.z, b.direction.z)); // ORTHO pick rays are parallel
    CHECK(length(sub(a.origin, b.origin)) > 1.0f); // ...and genuinely distinct rays

    const View solid = fixture_view_3d();
    const Ray c = pick_ray(solid, RegionPoint{200, 300}, target());
    const Ray d = pick_ray(solid, RegionPoint{1700, 800}, target());
    CHECK(length(sub(c.direction, d.direction)) > 0.1f); // PERSPECTIVE pick rays diverge
    CHECK(length(sub(c.origin, d.origin)) < 1.0f);       // ...from nearly the same eye point
}

// ---------------------------------------------------------------------------------------------
// Degenerate / zero-extent views stay finite (the test_degenerate_camera_is_finite precedent)
// ---------------------------------------------------------------------------------------------

void assert_view_is_finite(const View& v, Extent2D size)
{
    CHECK(finite_mat(view_matrix(v)));
    CHECK(finite_mat(projection_matrix(v, size)));
    CHECK(finite_mat(view_proj(v, size)));
    CHECK(finite3(project(v, size, Vec3{1.5f, -2.25f, 3.75f})));
    CHECK(finite3(unproject(v, size, Vec3{0.25f, -0.5f, 0.75f})));
    const Ray r = pick_ray(v, RegionPoint{7, 11}, size);
    CHECK(finite3(r.origin) && finite3(r.direction));
}

void test_degenerate_views_are_finite()
{
    // A zero-extent region: a viewport panel mid-resize, or a minimized window.
    assert_view_is_finite(fixture_view_3d(), Extent2D{0u, 0u});
    assert_view_is_finite(fixture_view_2d(), Extent2D{0u, 0u});

    // A zero field of view / zero-height 2D framing / collapsed depth range.
    View pinhole = fixture_view_3d();
    pinhole.projection.fov_y_radians = 0.0f;
    assert_view_is_finite(pinhole, target());

    View flat = fixture_view_2d();
    flat.projection.ortho_half_height = 0.0f;
    assert_view_is_finite(flat, target());

    View collapsed = fixture_view_3d();
    collapsed.projection.near_z = 4.0f;
    collapsed.projection.far_z = 4.0f;
    assert_view_is_finite(collapsed, target());

    // An unset / zero rotation quaternion, which an authored scene can legitimately carry.
    View unrotated = fixture_view_3d();
    unrotated.transform.rotation[0] = 0.0f;
    unrotated.transform.rotation[1] = 0.0f;
    unrotated.transform.rotation[2] = 0.0f;
    unrotated.transform.rotation[3] = 0.0f;
    assert_view_is_finite(unrotated, target());
}

} // namespace

int main()
{
    test_view_defaults_and_fields();
    test_aspect_ratio_and_its_degenerate_guard();
    test_view_matrix_is_the_inverse_of_the_camera_placement();
    test_projection_pins_frustum_edges();
    test_round_trip_3d();
    test_round_trip_2d();
    test_two_d_mode_is_orthographic();
    test_pick_ray_passes_through_the_picked_point();
    test_pick_ray_flips_the_pixel_y_axis();
    test_pick_ray_is_parallel_in_two_d_and_divergent_in_three_d();
    test_degenerate_views_are_finite();
    RENDER_TEST_MAIN_END();
}
