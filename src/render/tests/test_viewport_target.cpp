// The M9 e11b per-viewport render-target registry (context/render/viewport_target.h), driven
// GPU-free over the fake RHI backend.
//
// Every load-bearing claim here is a RECLAMATION one -- a released slot is REUSED, a released
// texture is DESTROYED -- and reclamation is the assertion class that goes vacuous most easily: the
// APPEND-ONLY sibling registry this replaces satisfies "the call returned", "the handle is valid" and
// "count() did not shrink" perfectly while leaking a texture per resize. So every assertion below is
// a POSITIVE artifact with an exact value: the handle a reused slot mints, the exact number of
// device textures alive, the exact cumulative allocation count. The two counters are deliberately
// INDEPENDENT of each other -- textures_created() is the registry's own bookkeeping and
// g_live_fake_textures counts the objects themselves -- because a leak satisfies the first alone and
// a silent no-op satisfies the second alone, while only a correct implementation satisfies both.

#include "context/render/viewport_target.h"

#include "render_test.h"
#include "render_test_rhi.h"

#include <cstdint>
#include <memory>

using namespace context::render;

namespace
{

// The pixel size the DEVICE actually gave a texture, read off the fake. Distinct from
// ViewportTargetRegistry::size_of(), which reports the registry's own bookkeeping, so the two are
// independent observations of one claim.
//
// It is also the only SOUND way to ask "is this a different texture than the one I saw before".
// Comparing raw ITexture* against a pointer captured earlier is an ABA trap: destroying the old pair
// and allocating a new one routinely hands back the SAME heap addresses, so `get_color(a) !=
// before` FAILS against a perfectly correct implementation. Measured here -- that assertion was the
// first cut of the resize test and it reddened on the first run, on code that was reallocating
// exactly as intended. (A pointer comparison between two SIMULTANEOUSLY LIVE textures is fine and is
// still used below; it is only the across-a-free comparison that is unsound. Reading a captured
// pointer after its texture is gone would be worse still -- a use-after-free -- so nothing here
// holds one across a resize or a release.)
[[nodiscard]] Extent2D device_size_of(const ITexture* texture)
{
    if (texture == nullptr)
    {
        return Extent2D{};
    }
    return static_cast<const rendertest::FakeTexture*>(texture)->size();
}

// The FORMAT the device gave a texture. A viewport target's two attachments differ only in format and
// usage, so without this an implementation that allocated the colour texture TWICE would satisfy every
// count, size and pointer-distinctness assertion in this file.
[[nodiscard]] TextureFormat device_format_of(const ITexture* texture)
{
    return static_cast<const rendertest::FakeTexture*>(texture)->format();
}

void test_create_allocates_a_colour_and_a_depth_attachment()
{
    rendertest::FakeDevice device;
    const int base_live = rendertest::g_live_fake_textures;

    ViewportTargetRegistry registry(device);
    CHECK(registry.live_targets() == 0u);
    CHECK(registry.slot_count() == 0u);
    CHECK(registry.textures_created() == 0u);
    CHECK(!registry.contains(kInvalidViewportTarget));
    CHECK(registry.get_color(kInvalidViewportTarget) == nullptr);

    const ViewportTargetId a = registry.create(Extent2D{128, 64});
    CHECK(a != kInvalidViewportTarget);
    CHECK(viewport_target_slot(a) == 1u);       // slots are 1-based so 0 stays the invalid handle
    CHECK(viewport_target_generation(a) == 0u); // a fresh slot has never been recycled

    // ONE target is exactly TWO device textures: the colour attachment plus the Depth32Float one a
    // 3D viewport depth-sorts against.
    CHECK(registry.live_targets() == 1u);
    CHECK(registry.slot_count() == 1u);
    CHECK(registry.textures_created() == 2u);
    CHECK(rendertest::g_live_fake_textures - base_live == 2);

    CHECK(registry.contains(a));
    CHECK(registry.get_color(a) != nullptr);
    CHECK(registry.get_depth(a) != nullptr);
    CHECK(registry.color_view(a) != nullptr);
    CHECK(registry.depth_view(a) != nullptr);
    CHECK(registry.get_color(a) != registry.get_depth(a));
    CHECK(registry.size_of(a).width == 128u);
    CHECK(registry.size_of(a).height == 64u);
    // The two attachments are the right KINDS. Depth32Float is the only depth format the T1 RHI has,
    // and a colour texture cannot serve as a depth attachment -- but two colour textures would pass
    // every count and size assertion above.
    CHECK(device_format_of(registry.get_color(a)) == TextureFormat::RGBA8Unorm);
    CHECK(device_format_of(registry.get_depth(a)) == TextureFormat::Depth32Float);
    CHECK(device_size_of(registry.get_color(a)).width == 128u);
    CHECK(device_size_of(registry.get_depth(a)).height == 64u);

    // A DEGENERATE extent is refused rather than allocated: a panel mid-resize and a minimized window
    // are real transient states, and rhi.h's ISwapchain::resize takes the same position.
    CHECK(registry.create(Extent2D{0, 64}) == kInvalidViewportTarget);
    CHECK(registry.create(Extent2D{64, 0}) == kInvalidViewportTarget);
    CHECK(registry.create(Extent2D{0, 0}) == kInvalidViewportTarget);
    // ...and refusing allocated NOTHING: both counters are untouched, so the refusal is not merely a
    // return value that still burned a slot and two textures.
    CHECK(registry.live_targets() == 1u);
    CHECK(registry.slot_count() == 1u);
    CHECK(registry.textures_created() == 2u);
    CHECK(rendertest::g_live_fake_textures - base_live == 2);
}

void test_release_frees_the_slot_and_destroys_the_textures()
{
    rendertest::FakeDevice device;
    const int base_live = rendertest::g_live_fake_textures;

    ViewportTargetRegistry registry(device);
    const ViewportTargetId a = registry.create(Extent2D{64, 64});
    const ViewportTargetId b = registry.create(Extent2D{32, 32});
    CHECK(a != kInvalidViewportTarget && b != kInvalidViewportTarget);
    CHECK(registry.live_targets() == 2u);
    CHECK(rendertest::g_live_fake_textures - base_live == 4);

    CHECK(registry.release(a));

    // The POSITIVE artifact: the two textures a held are GONE from the device, exactly two of the
    // four. `live_targets()` going down is the registry's own report; this is the device's.
    CHECK(rendertest::g_live_fake_textures - base_live == 2);
    CHECK(registry.live_targets() == 1u);
    // The SLOT survives (it is on the free list, ready to be recycled) -- so a shrinking
    // live_targets() next to a stable slot_count() is what "freed for reuse" looks like.
    CHECK(registry.slot_count() == 2u);

    // b is untouched by a's release, and still resolves at its OWN size.
    CHECK(registry.contains(b));
    CHECK(registry.get_color(b) != nullptr);
    CHECK(registry.size_of(b).width == 32u);
}

void test_a_reused_slot_mints_a_new_handle_and_refuses_the_stale_one()
{
    rendertest::FakeDevice device;
    ViewportTargetRegistry registry(device);

    const ViewportTargetId a = registry.create(Extent2D{32, 32});
    CHECK(registry.release(a));
    const ViewportTargetId b = registry.create(Extent2D{48, 96});

    // BOTH halves, because either one alone is satisfied by a wrong implementation: the append-only
    // sibling passes the generation half for free (it never reuses, so no handle ever repeats), and a
    // generation-less reuse passes the slot half for free (and then aliases stale handles onto the
    // new target). Only reuse WITH a generation bump satisfies both.
    CHECK(viewport_target_slot(b) == viewport_target_slot(a)); // the slot really was recycled
    CHECK(viewport_target_generation(b) == viewport_target_generation(a) + 1u);
    CHECK(b != a);

    // The stale handle is refused everywhere...
    CHECK(!registry.contains(a));
    CHECK(registry.get_color(a) == nullptr);
    CHECK(registry.get_depth(a) == nullptr);
    CHECK(registry.color_view(a) == nullptr);
    CHECK(registry.depth_view(a) == nullptr);
    CHECK(registry.size_of(a).width == 0u);
    CHECK(registry.size_of(a).height == 0u);
    // ...while the live handle resolves, at ITS size. This is the positive counterpart that stops the
    // refusals above from passing on a registry that resolves nothing at all.
    CHECK(registry.contains(b));
    CHECK(registry.get_color(b) != nullptr);
    CHECK(registry.size_of(b).width == 48u);
    CHECK(registry.size_of(b).height == 96u);
    CHECK(registry.slot_count() == 1u); // one slot, used twice -- not two slots
    CHECK(registry.live_targets() == 1u);
}

void test_a_double_release_is_refused_and_frees_the_slot_exactly_once()
{
    rendertest::FakeDevice device;
    ViewportTargetRegistry registry(device);

    const ViewportTargetId a = registry.create(Extent2D{16, 16});
    CHECK(registry.release(a));
    CHECK(!registry.release(a));                   // the second release is refused...
    CHECK(!registry.release(kInvalidViewportTarget));
    CHECK(!registry.release(0xDEADBEEFu));

    // ...and the POSITIVE proof that the refusal actually protected the free list: b recycles a's
    // slot, and c must then be a BRAND NEW slot. A release that pushed the slot twice would hand the
    // same entry to both, so two live targets would share one set of textures.
    const ViewportTargetId b = registry.create(Extent2D{16, 16});
    const ViewportTargetId c = registry.create(Extent2D{16, 16});
    CHECK(viewport_target_slot(b) == viewport_target_slot(a));
    CHECK(viewport_target_slot(c) != viewport_target_slot(a));
    CHECK(viewport_target_slot(c) == 2u);
    CHECK(registry.slot_count() == 2u);
    CHECK(registry.live_targets() == 2u);
    CHECK(registry.get_color(b) != registry.get_color(c)); // two targets, two colour textures
}

void test_resize_keeps_the_handle_and_destroys_the_old_attachments()
{
    rendertest::FakeDevice device;
    const int base_live = rendertest::g_live_fake_textures;

    ViewportTargetRegistry registry(device);
    const ViewportTargetId a = registry.create(Extent2D{64, 64});
    CHECK(registry.get_color(a) != nullptr && registry.get_depth(a) != nullptr);
    CHECK(device_size_of(registry.get_color(a)).width == 64u);
    CHECK(device_size_of(registry.get_depth(a)).width == 64u);

    CHECK(registry.resize(a, Extent2D{256, 128}));

    // The handle is UNCHANGED -- the viewport did not change identity, only extent. This is the exact
    // property the append-only sibling cannot offer, where the only way to change size is to mint
    // another handle and abandon the old one.
    CHECK(registry.contains(a));
    CHECK(registry.size_of(a).width == 256u);
    CHECK(registry.size_of(a).height == 128u);
    // Both attachments really were reallocated AT THE NEW EXTENT, read off the device rather than off
    // the registry's own bookkeeping. (Not `get_color(a) != first_color` -- see device_size_of().)
    CHECK(device_size_of(registry.get_color(a)).width == 256u);
    CHECK(device_size_of(registry.get_color(a)).height == 128u);
    CHECK(device_size_of(registry.get_depth(a)).width == 256u);
    CHECK(device_size_of(registry.get_depth(a)).height == 128u);
    CHECK(registry.textures_created() == 4u); // 2 at create + 2 at resize
    // ...and the OLD pair really was destroyed. Two counters, opposite failure modes: a leak keeps
    // textures_created() at 4 while the live count climbs to 4; a no-op keeps the live count at 2
    // while textures_created() stays at 2 and the size assertions above fail.
    CHECK(rendertest::g_live_fake_textures - base_live == 2);

    CHECK(!registry.resize(kInvalidViewportTarget, Extent2D{8, 8}));
    CHECK(!registry.resize(0xBADF00Du, Extent2D{8, 8}));
}

void test_resize_to_the_same_size_does_not_reallocate()
{
    rendertest::FakeDevice device;
    ViewportTargetRegistry registry(device);

    const ViewportTargetId a = registry.create(Extent2D{100, 50});
    const ITexture* color = registry.get_color(a);
    const ITexture* depth = registry.get_depth(a);

    // A viewport calls resize every frame with whatever its panel currently measures, so the
    // unchanged case is the COMMON one. Reallocating there would churn two device textures per frame.
    CHECK(registry.resize(a, Extent2D{100, 50}));
    CHECK(registry.resize(a, Extent2D{100, 50}));
    CHECK(registry.textures_created() == 2u); // still just the create's pair
    CHECK(registry.get_color(a) == color);    // the SAME objects, not merely the same size
    CHECK(registry.get_depth(a) == depth);
    CHECK(registry.size_of(a).width == 100u);

    // A real change still reallocates, so the early-out above is not just "resize does nothing".
    // Asserted through the DEVICE's extent, not against the `color` pointer -- that one is dangling
    // the moment the old texture is freed (see device_size_of()).
    CHECK(registry.resize(a, Extent2D{100, 51}));
    CHECK(registry.textures_created() == 4u);
    CHECK(registry.size_of(a).height == 51u);
    CHECK(device_size_of(registry.get_color(a)).height == 51u);
    CHECK(device_size_of(registry.get_depth(a)).height == 51u);
}

void test_resize_to_a_degenerate_extent_keeps_the_last_valid_configuration()
{
    rendertest::FakeDevice device;
    ViewportTargetRegistry registry(device);

    const ViewportTargetId a = registry.create(Extent2D{80, 40});
    const ITexture* color = registry.get_color(a);

    // A minimized window reports a zero extent. Tearing the target down there would make the next
    // restore reallocate; rhi.h's ISwapchain::resize documents the same choice for a swapchain.
    CHECK(registry.resize(a, Extent2D{0, 40}));
    CHECK(registry.resize(a, Extent2D{80, 0}));
    CHECK(registry.resize(a, Extent2D{0, 0}));
    CHECK(registry.contains(a));
    CHECK(registry.get_color(a) == color); // the SAME texture, not a 0x0 replacement
    CHECK(registry.size_of(a).width == 80u);
    CHECK(registry.size_of(a).height == 40u);
    CHECK(registry.textures_created() == 2u);
}

void test_acquire_for_holds_one_target_across_a_resize_storm()
{
    // THE defect this registry exists to remove, stated as a test: on the append-only sibling a
    // per-viewport RT "leaks a handle per resize", because create_panel_target() is the only way to
    // change size. Dragging a panel edge produces one of these per frame.
    rendertest::FakeDevice device;
    const int base_live = rendertest::g_live_fake_textures;

    ViewportTargetRegistry registry(device);
    const ViewportTargetId first = registry.acquire_for(7u, Extent2D{320, 200});
    CHECK(first != kInvalidViewportTarget);
    CHECK(registry.target_for(7u) == first);
    CHECK(rendertest::g_live_fake_textures - base_live == 2);

    constexpr std::uint32_t kResizes = 20u;
    for (std::uint32_t i = 1; i <= kResizes; ++i)
    {
        const ViewportTargetId again = registry.acquire_for(7u, Extent2D{320u + i, 200u + i});
        CHECK(again == first); // the SAME handle every frame -- not merely a valid one
    }

    // Exact values on both axes. A leak gives live == 2 * 21 = 42; a no-op gives
    // textures_created() == 2 and the wrong final size. Neither can satisfy all three.
    CHECK(registry.live_targets() == 1u);
    CHECK(registry.slot_count() == 1u);
    CHECK(rendertest::g_live_fake_textures - base_live == 2);
    CHECK(registry.textures_created() == 2u * (kResizes + 1u));
    CHECK(registry.size_of(first).width == 320u + kResizes);
    CHECK(registry.size_of(first).height == 200u + kResizes);

    // A SECOND viewport is a genuinely separate target -- acquire_for keys on the viewport, so this
    // rules out a one-slot registry passing everything above by holding a single global target.
    const ViewportTargetId other = registry.acquire_for(9u, Extent2D{64, 64});
    CHECK(other != kInvalidViewportTarget);
    CHECK(other != first);
    CHECK(registry.live_targets() == 2u);
    CHECK(registry.size_of(other).width == 64u);
    CHECK(registry.size_of(first).width == 320u + kResizes); // ...and did not disturb the first
    CHECK(rendertest::g_live_fake_textures - base_live == 4);
}

void test_release_for_frees_the_viewports_target_and_forgets_the_mapping()
{
    rendertest::FakeDevice device;
    const int base_live = rendertest::g_live_fake_textures;

    ViewportTargetRegistry registry(device);
    const ViewportTargetId a = registry.acquire_for(3u, Extent2D{128, 128});
    CHECK(a != kInvalidViewportTarget);
    CHECK(rendertest::g_live_fake_textures - base_live == 2);

    CHECK(registry.release_for(3u));
    CHECK(!registry.release_for(3u));  // the mapping is gone, so a second release finds nothing
    CHECK(!registry.release_for(99u)); // an unknown viewport was never bound
    CHECK(registry.target_for(3u) == kInvalidViewportTarget);
    CHECK(registry.live_targets() == 0u);
    CHECK(rendertest::g_live_fake_textures - base_live == 0); // a closed viewport gives its memory back

    // Re-opening that viewport recycles the slot and mints a FRESH handle, so nothing holding the old
    // one can address the new target.
    const ViewportTargetId b = registry.acquire_for(3u, Extent2D{64, 64});
    CHECK(b != kInvalidViewportTarget);
    CHECK(viewport_target_slot(b) == viewport_target_slot(a));
    CHECK(b != a);
    CHECK(!registry.contains(a));
    CHECK(registry.contains(b));
    CHECK(registry.slot_count() == 1u);
    CHECK(rendertest::g_live_fake_textures - base_live == 2);

    // Releasing the HANDLE directly must also drop the viewport binding, or target_for() would keep
    // handing out a handle that no longer resolves.
    CHECK(registry.release(b));
    CHECK(registry.target_for(3u) == kInvalidViewportTarget);
}

void test_acquire_for_never_allocates_on_a_degenerate_extent()
{
    rendertest::FakeDevice device;
    const int base_live = rendertest::g_live_fake_textures;

    ViewportTargetRegistry registry(device);
    // Nothing bound yet: there is no sensible size to allocate, so nothing is allocated.
    CHECK(registry.acquire_for(1u, Extent2D{0, 0}) == kInvalidViewportTarget);
    CHECK(registry.live_targets() == 0u);
    CHECK(registry.slot_count() == 0u);
    CHECK(rendertest::g_live_fake_textures - base_live == 0);

    // Already bound: the viewport keeps its last valid configuration through the minimize.
    const ViewportTargetId a = registry.acquire_for(1u, Extent2D{200, 100});
    CHECK(registry.acquire_for(1u, Extent2D{0, 100}) == a);
    CHECK(registry.acquire_for(1u, Extent2D{0, 0}) == a);
    CHECK(registry.size_of(a).width == 200u);
    CHECK(registry.textures_created() == 2u);
    CHECK(rendertest::g_live_fake_textures - base_live == 2);
}

void test_all_targets_are_destroyed_when_the_registry_dies()
{
    rendertest::FakeDevice device;
    const int base_live = rendertest::g_live_fake_textures;
    {
        ViewportTargetRegistry registry(device);
        registry.acquire_for(1u, Extent2D{64, 64});
        registry.acquire_for(2u, Extent2D{64, 64});
        registry.create(Extent2D{64, 64});
        CHECK(registry.live_targets() == 3u);
        CHECK(rendertest::g_live_fake_textures - base_live == 6);
    }
    CHECK(rendertest::g_live_fake_textures - base_live == 0);
}

} // namespace

int main()
{
    test_create_allocates_a_colour_and_a_depth_attachment();
    test_release_frees_the_slot_and_destroys_the_textures();
    test_a_reused_slot_mints_a_new_handle_and_refuses_the_stale_one();
    test_a_double_release_is_refused_and_frees_the_slot_exactly_once();
    test_resize_keeps_the_handle_and_destroys_the_old_attachments();
    test_resize_to_the_same_size_does_not_reallocate();
    test_resize_to_a_degenerate_extent_keeps_the_last_valid_configuration();
    test_acquire_for_holds_one_target_across_a_resize_storm();
    test_release_for_frees_the_viewports_target_and_forgets_the_mapping();
    test_acquire_for_never_allocates_on_a_degenerate_extent();
    test_all_targets_are_destroyed_when_the_registry_dies();
    RENDER_TEST_MAIN_END();
}
