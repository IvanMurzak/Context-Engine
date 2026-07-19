// The CPU present fallback — see present_blit.h.

#include "context/render/present/present_blit.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace context::render::present
{

BlitPlan compute_blit_plan(Extent2D src, Extent2D dst)
{
    BlitPlan plan;
    if (src.width == 0 || src.height == 0 || dst.width == 0 || dst.height == 0)
    {
        return plan; // empty stays true — a minimized window presents nothing
    }

    // Fit the source inside the destination preserving aspect. Compare src.w/src.h against
    // dst.w/dst.h by cross-multiplying, so the choice is made in exact integer arithmetic.
    const std::uint64_t src_aspect = static_cast<std::uint64_t>(src.width) * dst.height;
    const std::uint64_t dst_aspect = static_cast<std::uint64_t>(dst.width) * src.height;

    if (src_aspect > dst_aspect)
    {
        // Source is relatively wider: full destination width, bars top and bottom.
        plan.width = dst.width;
        plan.height = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(1u, static_cast<std::uint64_t>(dst.width) * src.height /
                                            src.width));
    }
    else if (src_aspect < dst_aspect)
    {
        // Source is relatively taller: full destination height, bars left and right.
        plan.height = dst.height;
        plan.width = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(1u, static_cast<std::uint64_t>(dst.height) * src.width /
                                            src.height));
    }
    else
    {
        plan.width = dst.width;
        plan.height = dst.height;
    }

    plan.width = std::min(plan.width, dst.width);
    plan.height = std::min(plan.height, dst.height);
    plan.x = static_cast<std::int32_t>((dst.width - plan.width) / 2u);
    plan.y = static_cast<std::int32_t>((dst.height - plan.height) / 2u);
    plan.letterboxed = plan.width != dst.width || plan.height != dst.height;
    plan.empty = false;
    return plan;
}

bool MemoryBlitter::blit(const BlitImage& src, Extent2D dst)
{
    const BlitPlan plan = compute_blit_plan(src.size, dst);
    last_plan_ = plan;
    if (plan.empty || src.pixels == nullptr || src.bytes_per_row < src.size.width * 4u)
    {
        return false;
    }

    target_size_ = dst;
    target_.assign(static_cast<std::size_t>(dst.width) * dst.height * 4u, 0u);
    const auto* base = static_cast<const std::uint8_t*>(src.pixels);

    for (std::uint32_t y = 0; y < plan.height; ++y)
    {
        // Nearest-neighbour: map the destination pixel centre back into the source.
        const std::uint32_t sy = std::min(
            src.size.height - 1u,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * src.size.height) /
                                       plan.height));
        for (std::uint32_t x = 0; x < plan.width; ++x)
        {
            const std::uint32_t sx = std::min(
                src.size.width - 1u,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * src.size.width) /
                                           plan.width));
            const std::uint8_t* s =
                base + static_cast<std::size_t>(sy) * src.bytes_per_row + sx * 4u;
            const std::size_t dst_index =
                (static_cast<std::size_t>(static_cast<std::uint32_t>(plan.y) + y) * dst.width +
                 static_cast<std::uint32_t>(plan.x) + x) *
                4u;
            if (dst_index + 3u >= target_.size())
            {
                continue;
            }
            // Source is BGRA, target is RGBA — the same swizzle the sampler does on the GPU path.
            target_[dst_index + 0] = s[2];
            target_[dst_index + 1] = s[1];
            target_[dst_index + 2] = s[0];
            target_[dst_index + 3] = s[3];
        }
    }
    ++blit_count_;
    return true;
}

#if defined(_WIN32)
namespace
{

// GDI present: StretchDIBits the BGRA buffer straight into the window's DC. This is the ultimate
// degrade — it needs no GPU, no D3D device and no swapchain, which is exactly the point.
class Win32GdiBlitter final : public IPresentBlitter
{
public:
    explicit Win32GdiBlitter(HWND hwnd) : hwnd_(hwnd) {}

    [[nodiscard]] const char* name() const override { return "win32-gdi"; }

    bool blit(const BlitImage& src, Extent2D dst) override
    {
        const BlitPlan plan = compute_blit_plan(src.size, dst);
        if (plan.empty || src.pixels == nullptr || src.bytes_per_row < src.size.width * 4u)
        {
            return false;
        }

        HDC dc = ::GetDC(hwnd_);
        if (dc == nullptr)
        {
            return false;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(src.size.width);
        // NEGATIVE height = a top-down DIB, matching the producer's row order. A positive height
        // would present the whole UI upside down.
        info.bmiHeader.biHeight = -static_cast<LONG>(src.size.height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        // A padded source stride cannot be expressed in BITMAPINFO, so a padded frame is repacked
        // tightly first rather than presented with skewed rows.
        const std::uint32_t tight_row = src.size.width * 4u;
        const void* pixels = src.pixels;
        if (src.bytes_per_row != tight_row)
        {
            repack_.assign(static_cast<std::size_t>(tight_row) * src.size.height, 0u);
            const auto* base = static_cast<const std::uint8_t*>(src.pixels);
            for (std::uint32_t y = 0; y < src.size.height; ++y)
            {
                std::copy_n(base + static_cast<std::size_t>(y) * src.bytes_per_row, tight_row,
                            repack_.data() + static_cast<std::size_t>(y) * tight_row);
            }
            pixels = repack_.data();
        }

        // Bars first, so a letterboxed present does not leave the previous frame's edges on screen.
        if (plan.letterboxed)
        {
            RECT full{0, 0, static_cast<LONG>(dst.width), static_cast<LONG>(dst.height)};
            ::FillRect(dc, &full, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        }

        ::SetStretchBltMode(dc, HALFTONE);
        const int written = ::StretchDIBits(
            dc, plan.x, plan.y, static_cast<int>(plan.width), static_cast<int>(plan.height), 0, 0,
            static_cast<int>(src.size.width), static_cast<int>(src.size.height), pixels, &info,
            DIB_RGB_COLORS, SRCCOPY);
        ::ReleaseDC(hwnd_, dc);
        // StretchDIBits returns the scan lines copied, or GDI_ERROR (0xFFFFFFFF, i.e. -1 once it
        // lands in the documented `int` return). Testing `> 0` covers both failure spellings
        // without comparing an int against an unsigned macro (-Wsign-compare under /W4 and -Wextra).
        return written > 0;
    }

private:
    HWND hwnd_;
    std::vector<std::uint8_t> repack_;
};

} // namespace
#endif // _WIN32

std::unique_ptr<IPresentBlitter> make_win32_gdi_blitter(void* hwnd)
{
#if defined(_WIN32)
    if (hwnd == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<Win32GdiBlitter>(static_cast<HWND>(hwnd));
#else
    (void)hwnd;
    return nullptr;
#endif
}

BlitterSelection make_present_blitter(PresentPlatform platform, void* native_handle)
{
    BlitterSelection selection;
    if (native_handle == nullptr)
    {
        selection.diagnostic = "no native window handle: nothing to present into";
        return selection;
    }

    switch (platform)
    {
    case PresentPlatform::windows:
        selection.blitter = make_win32_gdi_blitter(native_handle);
        if (selection.blitter == nullptr)
        {
            selection.diagnostic =
                "the GDI blitter is compiled only on Windows; this build cannot present on win32";
        }
        return selection;
    case PresentPlatform::linux_:
        selection.diagnostic =
            "no X11 SHM present blitter in this build — the Linux CPU present fallback lands in e12";
        return selection;
    case PresentPlatform::macos:
        selection.diagnostic = "no CALayer.contents present blitter in this build — the macOS CPU "
                               "present fallback lands in e12";
        return selection;
    }
    selection.diagnostic = "unknown platform";
    return selection;
}

} // namespace context::render::present
