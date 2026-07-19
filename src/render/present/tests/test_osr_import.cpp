// The OSR import policy + dirty-rect upload driver (M9 e03; design 03 §3, L-41).
//
// Every platform branch is exercised HERE, on whatever OS this runs on, because select_osr_import
// takes the platform explicitly — the local Windows dev gate would otherwise never execute the macOS
// or Linux decision at all (the platform-`#if` blind spot).

#include "context/render/present/osr_import.h"

#include "render_test.h"
#include "render_test_rhi.h"

#include <string>
#include <vector>

using namespace context::render;
using namespace context::render::present;
using rendertest::mentions;
using rendertest::FakeDevice;
using rendertest::FakeQueue;
using rendertest::FakeTexture;

namespace
{

OsrImportOptions accelerated_offer()
{
    OsrImportOptions options;
    options.shared_texture_offered = true;
    return options;
}

// A synthetic CEF frame: `coded` allocation, BGRA8, with a per-pixel pattern so an upload landing at
// the wrong origin is visible rather than merely "different".
std::vector<std::uint8_t> make_frame(Extent2D coded, std::uint32_t bytes_per_row)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes_per_row) * coded.height, 0u);
    for (std::uint32_t y = 0; y < coded.height; ++y)
    {
        for (std::uint32_t x = 0; x < coded.width; ++x)
        {
            std::uint8_t* p = pixels.data() + static_cast<std::size_t>(y) * bytes_per_row + x * 4u;
            p[0] = static_cast<std::uint8_t>(x + 1u); // B
            p[1] = static_cast<std::uint8_t>(y + 1u); // G
            p[2] = 0x40;                              // R
            p[3] = 0xFF;                              // A
        }
    }
    return pixels;
}

// Describe `px` as a CPU-path producer frame. byte_size comes from the buffer itself, so a test
// cannot accidentally claim more bytes than it allocated.
OsrFrame cpu_frame(const std::vector<std::uint8_t>& px, Extent2D coded, std::uint32_t stride)
{
    OsrFrame frame;
    frame.pixels = px.data();
    frame.byte_size = px.size();
    frame.bytes_per_row = stride;
    frame.coded_size = coded;
    return frame;
}

// ---------------------------------------------------------------------- policy: the 3 platforms

void test_windows_policy_is_cpu_upload()
{
    // The owner ruling of 2026-07-19: no wgpu-native fork, so stock Windows has NO shared-handle
    // import and the Editor runs the CPU-upload path there.
    const OsrImportDecision d = select_osr_import(PresentPlatform::windows, accelerated_offer());
    CHECK(d.source == ExternalTextureSource::CpuBgra);
    CHECK(!d.accelerated());
    // The refusal names the upstream ask, so "why is Windows slow?" is answerable from the artifact.
    CHECK(mentions(d.rationale, "wgpu-native#621"));

    // ...and the accelerated branch is DORMANT, not deleted: flipping the one availability predicate
    // (what lands when the upstream C API gains import) restores it with no other change.
    OsrImportOptions upstream_landed = accelerated_offer();
    upstream_landed.windows_shared_handle_import_available = true;
    const OsrImportDecision restored = select_osr_import(PresentPlatform::windows, upstream_landed);
    CHECK(restored.source == ExternalTextureSource::D3D12SharedHandle);
    CHECK(restored.accelerated());
}

void test_macos_policy_is_accelerated()
{
    const OsrImportDecision d = select_osr_import(PresentPlatform::macos, accelerated_offer());
    CHECK(d.source == ExternalTextureSource::IOSurface);
    CHECK(d.accelerated());
    CHECK(mentions(d.rationale, "IOSurface"));
}

void test_linux_policy_is_gated()
{
    // Ships OFF.
    const OsrImportDecision closed = select_osr_import(PresentPlatform::linux_, accelerated_offer());
    CHECK(closed.source == ExternalTextureSource::CpuBgra);
    CHECK(!closed.accelerated());

    OsrImportOptions gated = accelerated_offer();
    gated.linux_dmabuf_gate = true;
    const OsrImportDecision open = select_osr_import(PresentPlatform::linux_, gated);
    CHECK(open.source == ExternalTextureSource::DmaBuf);
    CHECK(open.accelerated());
}

void test_force_software_beats_every_platform()
{
    // The ONE L-41 switch. It must win on the platform that would otherwise accelerate.
    OsrImportOptions forced = accelerated_offer();
    forced.force_software = true;
    for (PresentPlatform platform :
         {PresentPlatform::windows, PresentPlatform::macos, PresentPlatform::linux_})
    {
        const OsrImportDecision d = select_osr_import(platform, forced);
        CHECK(d.source == ExternalTextureSource::CpuBgra);
        CHECK(!d.accelerated());
    }
}

void test_software_osr_producer_never_accelerates()
{
    // No shared texture offered => there is nothing to import zero-copy, on any platform.
    const OsrImportOptions software; // shared_texture_offered defaults false
    for (PresentPlatform platform :
         {PresentPlatform::windows, PresentPlatform::macos, PresentPlatform::linux_})
    {
        CHECK(!select_osr_import(platform, software).accelerated());
    }
}

// ------------------------------------------------------------------------- clipping + uploads

void test_clip_rect()
{
    const Extent2D bounds{100, 50};
    Rect2D inside;
    inside.origin = {10, 10};
    inside.size = {20, 20};
    CHECK(clip_rect(inside, bounds).size.width == 20);

    Rect2D overhang;
    overhang.origin = {90, 40};
    overhang.size = {40, 40};
    const Rect2D clipped = clip_rect(overhang, bounds);
    CHECK(clipped.size.width == 10);
    CHECK(clipped.size.height == 10);

    Rect2D outside;
    outside.origin = {200, 0};
    outside.size = {10, 10};
    CHECK(clip_rect(outside, bounds).size.width == 0);
}

void test_dirty_rects_upload_only_the_damage()
{
    FakeDevice device;
    OsrTextureImporter importer(PresentPlatform::windows, accelerated_offer());
    CHECK(!importer.accelerated()); // Windows => CPU upload
    // The importer carries the inputs its decision came from, so a diagnostic report can explain
    // WHY a given box took a given path rather than only that it did.
    CHECK(importer.platform() == PresentPlatform::windows);
    CHECK(importer.options().shared_texture_offered);

    const Extent2D coded{64, 32};
    const std::uint32_t stride = coded.width * 4u;
    const std::vector<std::uint8_t> pixels = make_frame(coded, stride);

    OsrFrame frame = cpu_frame(pixels, coded, stride);
    frame.visible_rect.size = coded;

    // First paint: no damage reported => the whole visible rect.
    CHECK(importer.update(device, frame));
    CHECK(importer.texture() != nullptr);
    CHECK(importer.upload_count() == 1);

    auto& queue = static_cast<FakeQueue&>(device.queue());
    const std::uint64_t full_frame_texels = queue.written_texels();
    CHECK(full_frame_texels == static_cast<std::uint64_t>(coded.width) * coded.height);

    // Second paint: two small damaged rects. The whole point of the dirty-rect path is that this
    // moves FAR fewer texels than a full re-upload — assert it rather than assume it.
    Rect2D a;
    a.origin = {4, 4};
    a.size = {8, 8};
    Rect2D b;
    b.origin = {40, 16};
    b.size = {8, 8};
    frame.dirty = {a, b};
    CHECK(importer.update(device, frame));
    CHECK(importer.upload_count() == 3); // 1 + 2
    const std::uint64_t damage_texels = queue.written_texels() - full_frame_texels;
    CHECK(damage_texels == 128); // 8*8 twice
    CHECK(damage_texels * 4 < full_frame_texels);

    // The re-upload must not have reallocated: one import for two paints.
    CHECK(importer.import_count() == 1);
    CHECK(importer.frame_count() == 2);
}

void test_dirty_rect_lands_at_its_origin()
{
    // A sub-rect upload that ignored the origin would write the damage to the texture's top-left
    // corner and still "succeed" — so assert the destination texels, not just the call count.
    FakeDevice device;
    OsrTextureImporter importer(PresentPlatform::linux_, OsrImportOptions{});

    const Extent2D coded{16, 8};
    const std::uint32_t stride = coded.width * 4u;
    const std::vector<std::uint8_t> pixels = make_frame(coded, stride);

    OsrFrame frame = cpu_frame(pixels, coded, stride);
    Rect2D damage;
    damage.origin = {8, 4};
    damage.size = {4, 2};
    frame.dirty = {damage};

    CHECK(importer.update(device, frame));
    auto* texture = static_cast<FakeTexture*>(importer.texture());
    CHECK(texture != nullptr);
    if (texture != nullptr)
    {
        const std::vector<std::uint8_t>& px = texture->pixels();
        // Source texel (8,4) carries B = x+1 = 9, G = y+1 = 5 and must land at (8,4), not (0,0).
        const std::size_t at = (static_cast<std::size_t>(4) * coded.width + 8) * 4u;
        CHECK(px[at + 0] == 9);
        CHECK(px[at + 1] == 5);
        // The untouched corner stays zero.
        CHECK(px[0] == 0);
        CHECK(px[1] == 0);
    }
}

void test_resize_reimports()
{
    FakeDevice device;
    OsrTextureImporter importer(PresentPlatform::windows, OsrImportOptions{});

    Extent2D coded{16, 16};
    std::uint32_t stride = coded.width * 4u;
    std::vector<std::uint8_t> pixels = make_frame(coded, stride);
    OsrFrame frame = cpu_frame(pixels, coded, stride);
    CHECK(importer.update(device, frame));
    CHECK(importer.import_count() == 1);
    CHECK(importer.coded_size().width == 16);

    coded = {32, 16};
    stride = coded.width * 4u;
    pixels = make_frame(coded, stride);
    frame.pixels = pixels.data();
    frame.byte_size = pixels.size();
    frame.bytes_per_row = stride;
    frame.coded_size = coded;
    CHECK(importer.update(device, frame));
    CHECK(importer.import_count() == 2); // the allocation changed => a fresh import
    CHECK(importer.coded_size().width == 32);
}

void test_padded_stride_is_honoured()
{
    // CEF frames are commonly padded; using the rect width as the stride would skew every row.
    FakeDevice device;
    OsrTextureImporter importer(PresentPlatform::windows, OsrImportOptions{});

    const Extent2D coded{8, 4};
    const std::uint32_t stride = coded.width * 4u + 16u; // padded
    const std::vector<std::uint8_t> pixels = make_frame(coded, stride);

    OsrFrame frame = cpu_frame(pixels, coded, stride);
    CHECK(importer.update(device, frame));

    auto* texture = static_cast<FakeTexture*>(importer.texture());
    CHECK(texture != nullptr);
    if (texture != nullptr)
    {
        // Row 2, column 3 => B = 4, G = 3. Wrong-stride handling shifts this.
        const std::size_t at = (static_cast<std::size_t>(2) * coded.width + 3) * 4u;
        CHECK(texture->pixels()[at + 0] == 4);
        CHECK(texture->pixels()[at + 1] == 3);
    }
}

// ------------------------------------------------------------------------------ failure paths

void test_malformed_frames_are_refused()
{
    FakeDevice device;
    OsrTextureImporter importer(PresentPlatform::windows, OsrImportOptions{});

    OsrFrame empty; // zero coded_size
    CHECK(!importer.update(device, empty));
    CHECK(mentions(importer.diagnostic(), "coded_size"));

    const Extent2D coded{8, 8};
    OsrFrame no_pixels;
    no_pixels.coded_size = coded;
    CHECK(!importer.update(device, no_pixels));
    CHECK(mentions(importer.diagnostic(), "pixels"));

    const std::vector<std::uint8_t> pixels = make_frame(coded, coded.width * 4u);
    OsrFrame narrow;
    narrow.coded_size = coded;
    narrow.pixels = pixels.data();
    narrow.byte_size = pixels.size();
    narrow.bytes_per_row = 4u; // narrower than a row
    CHECK(!importer.update(device, narrow));
    CHECK(mentions(importer.diagnostic(), "bytes_per_row"));

    // A dirty rect that fits the texture but runs past the END of the supplied buffer: a truncated
    // frame must be refused, not read out of bounds.
    OsrFrame truncated;
    truncated.coded_size = coded;
    truncated.pixels = pixels.data();
    truncated.byte_size = 16u; // far short of a full frame
    truncated.bytes_per_row = coded.width * 4u;
    CHECK(!importer.update(device, truncated));
    CHECK(mentions(importer.diagnostic(), "past the end"));
}

void test_accelerated_import_degrades_loudly()
{
    // macOS policy picks IOSurface; a backend that cannot honour it must degrade to CPU upload AND
    // report it — a silent degrade is the ~4x-per-paint regression nobody notices.
    FakeDevice device; // no accelerated source declared available
    OsrTextureImporter importer(PresentPlatform::macos, accelerated_offer());
    CHECK(importer.decision().accelerated());

    const Extent2D coded{8, 8};
    const std::vector<std::uint8_t> pixels = make_frame(coded, coded.width * 4u);
    OsrFrame frame = cpu_frame(pixels, coded, coded.width * 4u);

    CHECK(importer.update(device, frame));
    CHECK(importer.degraded());
    CHECK(!importer.accelerated());
    CHECK(mentions(importer.diagnostic(), "degraded"));
    CHECK(importer.texture() != nullptr);
    CHECK(importer.upload_count() == 1); // and it really did upload
}

void test_accelerated_import_uploads_nothing()
{
    FakeDevice device;
    device.set_accelerated_available(ExternalTextureSource::IOSurface);
    OsrTextureImporter importer(PresentPlatform::macos, accelerated_offer());

    OsrFrame frame;
    frame.coded_size = {8, 8};
    frame.shared_handle = &device; // any non-null handle

    CHECK(importer.update(device, frame));
    CHECK(importer.accelerated());
    CHECK(!importer.degraded());
    // Zero CPU upload: no texels crossed the bus.
    CHECK(importer.upload_count() == 0);
    CHECK(static_cast<FakeQueue&>(device.queue()).written_texels() == 0);

    // ...but the accelerated path is NOT one-shot on macOS: each paint delivers a fresh IOSurface
    // that must be blitted in. An importer that skipped this would freeze the UI on frame 1 while
    // still reporting success, so the refresh is counted per frame and asserted.
    CHECK(importer.refresh_count() == 1);
    CHECK(importer.update(device, frame));
    CHECK(importer.update(device, frame));
    CHECK(importer.refresh_count() == 3);
    CHECK(importer.import_count() == 1); // and it reallocated nothing
    CHECK(device.refresh_count() == 3);
}

void test_accelerated_refresh_failure_degrades_and_recovers()
{
    // On macOS the IMPORT only allocates — every genuine IOSurface/Metal failure surfaces on the
    // per-frame REFRESH. So the refresh is where the degrade has to happen too: without it the
    // importer would take the same failing branch on every later frame and freeze the UI on the
    // last good composite forever, which is precisely what the import-time degrade exists to stop.
    FakeDevice device;
    device.set_accelerated_available(ExternalTextureSource::IOSurface);
    OsrTextureImporter importer(PresentPlatform::macos, accelerated_offer());

    const Extent2D coded{8, 8};
    OsrFrame accelerated;
    accelerated.coded_size = coded;
    accelerated.shared_handle = &device;
    CHECK(importer.update(device, accelerated)); // a healthy accelerated frame first
    CHECK(importer.accelerated());

    // Now the producer hands over a frame the backend cannot blit.
    OsrFrame handleless;
    handleless.coded_size = coded;
    handleless.shared_handle = nullptr;
    CHECK(!importer.update(device, handleless));
    CHECK(importer.degraded());
    CHECK(!importer.accelerated());
    // The report names the failure AND the standing consequence, then says what to send instead.
    CHECK(mentions(importer.diagnostic(), "refresh"));
    CHECK(mentions(importer.diagnostic(), "degraded"));
    CHECK(mentions(importer.diagnostic(), "resend"));

    // THE POINT: the very next frame that carries pixels goes through on the CPU path. A reported
    // failure that never recovers would be no better than a silent one.
    const std::vector<std::uint8_t> pixels = make_frame(coded, coded.width * 4u);
    OsrFrame software = cpu_frame(pixels, coded, coded.width * 4u);
    CHECK(importer.update(device, software));
    CHECK(importer.upload_count() == 1);
    CHECK(device.refresh_count() == 1); // the failed refresh was never retried

    // A resize must NOT re-attempt the abandoned accelerated import (two wasted calls per resize).
    const int imports_before = importer.import_count();
    OsrFrame bigger = software;
    bigger.coded_size = {16, 16};
    const std::vector<std::uint8_t> bigger_pixels = make_frame(bigger.coded_size, 16u * 4u);
    bigger.pixels = bigger_pixels.data();
    bigger.byte_size = bigger_pixels.size();
    bigger.bytes_per_row = 16u * 4u;
    CHECK(importer.update(device, bigger));
    CHECK(importer.import_count() == imports_before + 1); // exactly one, not one-plus-a-retry
    CHECK(importer.degraded());                           // and the degrade is permanent
}

void test_import_failure_is_reported_without_a_false_degrade()
{
    // The seam's PRIMARY contract: an import that fails outright yields no texture and says why.
    // Distinct from a degrade — nothing was downgraded here, the device simply refused.
    FakeDevice device;
    device.set_import_always_fails(true);
    OsrTextureImporter importer(PresentPlatform::windows, OsrImportOptions{});

    const Extent2D coded{8, 8};
    const std::vector<std::uint8_t> pixels = make_frame(coded, coded.width * 4u);
    OsrFrame frame = cpu_frame(pixels, coded, coded.width * 4u);

    CHECK(!importer.update(device, frame));
    CHECK(importer.texture() == nullptr);
    CHECK(!importer.degraded()); // a refusal is not a degrade
    CHECK(mentions(importer.diagnostic(), "import failed"));
    CHECK(importer.upload_count() == 0);

    // Once the device recovers, so does the importer — and the stale failure text goes with it.
    device.set_import_always_fails(false);
    CHECK(importer.update(device, frame));
    CHECK(importer.texture() != nullptr);
    CHECK(importer.diagnostic().empty());
}

void test_diagnostic_does_not_outlive_its_frame()
{
    // diagnostic() is the support-report channel, so a one-off malformed paint must not make every
    // later healthy frame read as broken.
    FakeDevice device;
    OsrTextureImporter importer(PresentPlatform::windows, OsrImportOptions{});

    OsrFrame malformed; // zero coded_size
    CHECK(!importer.update(device, malformed));
    CHECK(!importer.diagnostic().empty());

    const Extent2D coded{8, 8};
    const std::vector<std::uint8_t> pixels = make_frame(coded, coded.width * 4u);
    OsrFrame healthy = cpu_frame(pixels, coded, coded.width * 4u);

    CHECK(importer.update(device, healthy));
    CHECK(!importer.degraded());
    CHECK(importer.diagnostic().empty());
}

} // namespace

int main()
{
    test_windows_policy_is_cpu_upload();
    test_macos_policy_is_accelerated();
    test_linux_policy_is_gated();
    test_force_software_beats_every_platform();
    test_software_osr_producer_never_accelerates();
    test_clip_rect();
    test_dirty_rects_upload_only_the_damage();
    test_dirty_rect_lands_at_its_origin();
    test_resize_reimports();
    test_padded_stride_is_honoured();
    test_malformed_frames_are_refused();
    test_accelerated_import_degrades_loudly();
    test_accelerated_import_uploads_nothing();
    test_accelerated_refresh_failure_degrades_and_recovers();
    test_import_failure_is_reported_without_a_false_degrade();
    test_diagnostic_does_not_outlive_its_frame();
    RENDER_TEST_MAIN_END();
}
