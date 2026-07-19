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

bool is_blit_source_readable(const BlitImage& src)
{
    // rows_fit carries the "trailing padding on the LAST row is optional" rule, shared with the
    // dirty-rect upload driver so the two cannot drift apart.
    return src.pixels != nullptr && rows_fit(src.size, src.bytes_per_row, src.byte_size);
}

bool repack_tight_into(const BlitImage& src, std::vector<std::uint8_t>& out)
{
    if (!is_blit_source_readable(src))
    {
        out.clear();
        return false;
    }
    const std::size_t tight_row = static_cast<std::size_t>(src.size.width) * 4u;
    // resize, not assign: every byte is overwritten by the copy below, so zero-filling first is
    // pure waste — and resize keeps the retained capacity across frames.
    out.resize(tight_row * src.size.height);
    const auto* base = static_cast<const std::uint8_t*>(src.pixels);
    for (std::uint32_t y = 0; y < src.size.height; ++y)
    {
        std::copy_n(base + static_cast<std::size_t>(y) * src.bytes_per_row, tight_row,
                    out.data() + static_cast<std::size_t>(y) * tight_row);
    }
    return true;
}

std::vector<std::uint8_t> repack_tight(const BlitImage& src)
{
    std::vector<std::uint8_t> packed;
    repack_tight_into(src, packed);
    return packed;
}

bool MemoryBlitter::blit(const BlitImage& src, Extent2D dst)
{
    const BlitPlan plan = compute_blit_plan(src.size, dst);
    last_plan_ = plan;
    if (plan.empty || !is_blit_source_readable(src))
    {
        // Drop the previous frame on a refusal. Keeping it would leave target() holding stale
        // pixels under a target_size() the caller believes is current — a silently wrong present.
        target_.clear();
        target_size_ = Extent2D{};
        return false;
    }

    target_size_ = dst;
    target_.resize(static_cast<std::size_t>(dst.width) * dst.height * 4u);
    if (plan.letterboxed)
    {
        // Only a letterboxed present leaves pixels the loop below does not write — the bars. An
        // unconditional zero-fill would memset the entire target every frame and then overwrite all
        // of it, which at 2560x1440 is ~15 MB of dead writes per paint.
        std::fill(target_.begin(), target_.end(), std::uint8_t{0});
    }
    const auto* base = static_cast<const std::uint8_t*>(src.pixels);

    for (std::uint32_t y = 0; y < plan.height; ++y)
    {
        // Nearest-neighbour, top-left convention: floor(dst * src / plan). (The composite oracle
        // samples pixel CENTRES instead — the two need not agree, since this path never validates
        // that one, but do not read one as evidence for the other.)
        const std::uint32_t sy = std::min(
            src.size.height - 1u,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * src.size.height) /
                                       plan.height));
        const std::uint8_t* src_row = base + static_cast<std::size_t>(sy) * src.bytes_per_row;
        // Both bases are loop-invariant in x. No per-pixel bounds check: compute_blit_plan
        // guarantees plan.{x,y} + plan.{width,height} <= dst, and target_ is exactly
        // dst.width * dst.height * 4, so the last index written is target_.size() - 4.
        std::uint8_t* dst_row =
            target_.data() + (static_cast<std::size_t>(static_cast<std::uint32_t>(plan.y) + y) *
                                  dst.width +
                              static_cast<std::uint32_t>(plan.x)) *
                                 4u;
        for (std::uint32_t x = 0; x < plan.width; ++x)
        {
            const std::uint32_t sx = std::min(
                src.size.width - 1u,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * src.size.width) /
                                           plan.width));
            const std::uint8_t* s = src_row + static_cast<std::size_t>(sx) * 4u;
            std::uint8_t* d = dst_row + static_cast<std::size_t>(x) * 4u;
            // Source is BGRA, target is RGBA — the same swizzle the sampler does on the GPU path.
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
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
        if (plan.empty || !is_blit_source_readable(src))
        {
            return false;
        }

        // Repack BEFORE acquiring the DC. A padded source stride cannot be expressed in a
        // BITMAPINFO, so a padded frame is repacked tightly rather than presented with skewed rows
        // — and doing it first means the allocation cannot throw while we are holding a DC that
        // only the single ReleaseDC below would free.
        const void* pixels = src.pixels;
        if (src.bytes_per_row != src.size.width * 4u)
        {
            // Into the RETAINED member buffer, not a fresh vector: repacking a 2560x1440 frame
            // through a by-value return would malloc + free ~15 MB every paint.
            if (!repack_tight_into(src, repack_))
            {
                return false;
            }
            pixels = repack_.data();
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

        // Bars first, so a letterboxed present does not leave the previous frame's edges on screen.
        if (plan.letterboxed)
        {
            RECT full{0, 0, static_cast<LONG>(dst.width), static_cast<LONG>(dst.height)};
            ::FillRect(dc, &full, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        }

        ::SetStretchBltMode(dc, HALFTONE);
        // MSDN: after selecting HALFTONE the brush origin must be re-set, or the halftone pattern
        // misaligns against the DC's brush.
        ::SetBrushOrgEx(dc, 0, 0, nullptr);
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
