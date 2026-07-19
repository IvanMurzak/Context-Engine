// The C-F2 CPU present fallback (M9 e03; design 03 §2, §7) — the GPU-less present path.
//
// The geometry is deliberately separated from every OS call, so the letterbox/pillarbox arithmetic
// that a Windows GDI StretchDIBits and a future X11 SHM blitter BOTH depend on is asserted on all
// three OSes here. MemoryBlitter runs the identical plan into a buffer, which is what turns "the
// image is centred and unstretched" into a pixel assertion rather than a claim.

#include "context/render/present/present_blit.h"

#include "render_test.h"

#include <string>
#include <vector>

using namespace context::render;
using namespace context::render::present;

namespace
{

bool mentions(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// A BGRA image whose every pixel encodes its own coordinates, so a mis-scaled or mis-offset blit is
// identifiable rather than merely wrong.
std::vector<std::uint8_t> coordinate_image(Extent2D size, std::uint32_t bytes_per_row)
{
    std::vector<std::uint8_t> px(static_cast<std::size_t>(bytes_per_row) * size.height, 0u);
    for (std::uint32_t y = 0; y < size.height; ++y)
    {
        for (std::uint32_t x = 0; x < size.width; ++x)
        {
            std::uint8_t* p = px.data() + static_cast<std::size_t>(y) * bytes_per_row + x * 4u;
            p[0] = static_cast<std::uint8_t>(x); // B
            p[1] = static_cast<std::uint8_t>(y); // G
            p[2] = 0x7F;                         // R
            p[3] = 0xFF;                         // A
        }
    }
    return px;
}

// ------------------------------------------------------------------------------------ geometry

void test_matching_aspect_fills_the_surface()
{
    const BlitPlan plan = compute_blit_plan(Extent2D{800, 600}, Extent2D{1600, 1200});
    CHECK(!plan.empty);
    CHECK(!plan.letterboxed);
    CHECK(plan.x == 0);
    CHECK(plan.y == 0);
    CHECK(plan.width == 1600);
    CHECK(plan.height == 1200);
}

void test_wider_source_letterboxes()
{
    // 2:1 source into a 1:1 surface => full width, bars top and bottom, vertically centred.
    const BlitPlan plan = compute_blit_plan(Extent2D{400, 200}, Extent2D{400, 400});
    CHECK(plan.letterboxed);
    CHECK(plan.width == 400);
    CHECK(plan.height == 200);
    CHECK(plan.x == 0);
    CHECK(plan.y == 100);
}

void test_taller_source_pillarboxes()
{
    const BlitPlan plan = compute_blit_plan(Extent2D{200, 400}, Extent2D{400, 400});
    CHECK(plan.letterboxed);
    CHECK(plan.width == 200);
    CHECK(plan.height == 400);
    CHECK(plan.x == 100);
    CHECK(plan.y == 0);
}

void test_degenerate_sizes_are_empty()
{
    // A minimized window is the everyday case here, and it must present nothing rather than
    // divide by zero.
    CHECK(compute_blit_plan(Extent2D{100, 100}, Extent2D{0, 0}).empty);
    CHECK(compute_blit_plan(Extent2D{0, 0}, Extent2D{100, 100}).empty);
    CHECK(compute_blit_plan(Extent2D{100, 0}, Extent2D{100, 100}).empty);

    // An extreme fit must still leave at least one pixel rather than collapsing to zero.
    const BlitPlan sliver = compute_blit_plan(Extent2D{1000, 1}, Extent2D{10, 10});
    CHECK(!sliver.empty);
    CHECK(sliver.height >= 1);
    CHECK(sliver.width <= 10);
}

void test_plan_never_exceeds_the_surface()
{
    const Extent2D dst{37, 91}; // deliberately awkward, to catch rounding that overshoots
    for (std::uint32_t w = 1; w <= 200; w += 7)
    {
        for (std::uint32_t h = 1; h <= 200; h += 11)
        {
            const BlitPlan plan = compute_blit_plan(Extent2D{w, h}, dst);
            CHECK(!plan.empty);
            CHECK(plan.width <= dst.width);
            CHECK(plan.height <= dst.height);
            CHECK(plan.x >= 0);
            CHECK(plan.y >= 0);
            CHECK(static_cast<std::uint32_t>(plan.x) + plan.width <= dst.width);
            CHECK(static_cast<std::uint32_t>(plan.y) + plan.height <= dst.height);
        }
    }
}

// -------------------------------------------------------------------------------- the blitter

void test_memory_blit_is_1_to_1_and_swizzled()
{
    const Extent2D size{4, 4};
    const std::vector<std::uint8_t> src = coordinate_image(size, size.width * 4u);

    BlitImage image;
    image.pixels = src.data();
    image.size = size;
    image.bytes_per_row = size.width * 4u;

    MemoryBlitter blitter;
    CHECK(blitter.blit(image, size));
    CHECK(blitter.blit_count() == 1);
    CHECK(std::string(blitter.name()) == "memory");
    CHECK(!blitter.last_plan().letterboxed);
    CHECK(blitter.target_size().width == 4);

    // Source (2,3) is BGRA (2,3,0x7F,0xFF); the RGBA target must read (0x7F,3,2,0xFF).
    const std::size_t at = (static_cast<std::size_t>(3) * size.width + 2) * 4u;
    CHECK(blitter.target()[at + 0] == 0x7F);
    CHECK(blitter.target()[at + 1] == 3);
    CHECK(blitter.target()[at + 2] == 2);
    CHECK(blitter.target()[at + 3] == 0xFF);
}

void test_memory_blit_centres_and_leaves_bars_clear()
{
    // 2:1 source into a square surface: the bars must stay untouched (zero), and the image must sit
    // exactly in the middle.
    const Extent2D src_size{4, 2};
    const Extent2D dst_size{4, 4};
    const std::vector<std::uint8_t> src = coordinate_image(src_size, src_size.width * 4u);

    BlitImage image;
    image.pixels = src.data();
    image.size = src_size;
    image.bytes_per_row = src_size.width * 4u;

    MemoryBlitter blitter;
    CHECK(blitter.blit(image, dst_size));
    CHECK(blitter.last_plan().letterboxed);
    CHECK(blitter.last_plan().y == 1);

    // Row 0 is a bar => untouched.
    CHECK(blitter.target()[3] == 0);
    // Row 1 is the first image row => opaque.
    const std::size_t row1 = static_cast<std::size_t>(dst_size.width) * 4u;
    CHECK(blitter.target()[row1 + 3] == 0xFF);
    // Row 3 is the bottom bar => untouched again.
    const std::size_t row3 = static_cast<std::size_t>(3) * dst_size.width * 4u;
    CHECK(blitter.target()[row3 + 3] == 0);
}

void test_memory_blit_scales()
{
    const Extent2D src_size{2, 2};
    const Extent2D dst_size{4, 4};
    const std::vector<std::uint8_t> src = coordinate_image(src_size, src_size.width * 4u);

    BlitImage image;
    image.pixels = src.data();
    image.size = src_size;
    image.bytes_per_row = src_size.width * 4u;

    MemoryBlitter blitter;
    CHECK(blitter.blit(image, dst_size));
    // 2x nearest-neighbour upscale: destination (3,3) samples source (1,1) => B=1, G=1.
    const std::size_t at = (static_cast<std::size_t>(3) * dst_size.width + 3) * 4u;
    CHECK(blitter.target()[at + 1] == 1); // G carries the source row
    CHECK(blitter.target()[at + 2] == 1); // B carries the source column
}

void test_memory_blit_refuses_malformed_input()
{
    MemoryBlitter blitter;
    BlitImage none;
    CHECK(!blitter.blit(none, Extent2D{4, 4}));

    const Extent2D size{4, 4};
    const std::vector<std::uint8_t> src = coordinate_image(size, size.width * 4u);
    BlitImage narrow;
    narrow.pixels = src.data();
    narrow.size = size;
    narrow.bytes_per_row = 4u; // narrower than one row
    CHECK(!blitter.blit(narrow, size));
    CHECK(blitter.blit_count() == 0);
}

// ------------------------------------------------------------------------- platform selection

void test_platform_selection_is_never_silent()
{
    int window = 0; // a stand-in handle; the non-Windows branches never dereference it

    // The platforms whose blitter e12 still owes must report WHY, so the Shell degrades loudly.
    const BlitterSelection linux_sel = make_present_blitter(PresentPlatform::linux_, &window);
    CHECK(linux_sel.blitter == nullptr);
    CHECK(mentions(linux_sel.diagnostic, "e12"));

    const BlitterSelection mac_sel = make_present_blitter(PresentPlatform::macos, &window);
    CHECK(mac_sel.blitter == nullptr);
    CHECK(mentions(mac_sel.diagnostic, "e12"));

    // No handle at all is also reported, not silently ignored.
    const BlitterSelection none = make_present_blitter(PresentPlatform::windows, nullptr);
    CHECK(none.blitter == nullptr);
    CHECK(!none.diagnostic.empty());

    // The Windows factory is compiled only on Windows; off-Windows it must refuse cleanly rather
    // than hand back something that cannot work.
#if defined(_WIN32)
    // A bogus HWND still yields a blitter object — GetDC failing is a runtime false, not a
    // construction failure, which is what keeps the Shell's degrade path simple.
    CHECK(make_win32_gdi_blitter(&window) != nullptr);
#else
    CHECK(make_win32_gdi_blitter(&window) == nullptr);
    const BlitterSelection win_sel = make_present_blitter(PresentPlatform::windows, &window);
    CHECK(win_sel.blitter == nullptr);
    CHECK(!win_sel.diagnostic.empty());
#endif
}

} // namespace

int main()
{
    test_matching_aspect_fills_the_surface();
    test_wider_source_letterboxes();
    test_taller_source_pillarboxes();
    test_degenerate_sizes_are_empty();
    test_plan_never_exceeds_the_surface();
    test_memory_blit_is_1_to_1_and_swizzled();
    test_memory_blit_centres_and_leaves_bars_clear();
    test_memory_blit_scales();
    test_memory_blit_refuses_malformed_input();
    test_platform_selection_is_never_silent();
    RENDER_TEST_MAIN_END();
}
