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

#if defined(__linux__) && defined(CONTEXT_PRESENT_HAS_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <bit>
#include <cstdint>

// X11/X.h macro-defines these one-word names, which collide with the rhi.h enumerators declared
// above (render::NativeWindowKind::None, render::CompareFunction::Always). Those declarations are
// already parsed, so undefining the macros here costs nothing and keeps any later use compiling.
#undef None
#undef Always
#undef Success
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

std::uint32_t blit_source_index(std::uint32_t offset_in_plan, std::uint32_t src_extent,
                                std::uint32_t plan_extent)
{
    if (plan_extent == 0u || src_extent == 0u)
    {
        return 0u;
    }
    // 64-bit intermediate: offset * src_extent overflows 32 bits well inside the supported window
    // sizes (a 30k-wide source at a 4k offset already does).
    return std::min(src_extent - 1u,
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(offset_in_plan) * src_extent) / plan_extent));
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
        const std::uint32_t sy = blit_source_index(y, src.size.height, plan.height);
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
            const std::uint32_t sx = blit_source_index(x, src.size.width, plan.width);
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

#if defined(__linux__) && defined(CONTEXT_PRESENT_HAS_X11)
namespace
{

// X11 present: scale the BGRA buffer into an XImage and push it to the window, through MIT-SHM when
// the server shares memory with us and plain XPutImage when it does not. Like the GDI path this
// needs no GPU, no EGL/GLX context and no swapchain, which is exactly the point.
//
// WHY THE PIXELS ARE COMPOSED FROM THE VISUAL'S MASKS rather than memcpy'd. A local 24/32-bit
// TrueColor visual on a little-endian box really is BGRX in memory, so a straight copy is right
// almost everywhere — and silently wrong on the rest (an RGBA visual), where it swaps red and blue
// on the whole UI. The per-pixel path costs one shift per channel on a path that already exists
// only because there is no GPU, and it gets any channel ORDER right.
//
// Two things it does NOT claim, both refused rather than approximated: a channel field that is not
// 8 bits wide (the depth-30 case), and a server whose byte order differs from ours. Both are
// properties of the (visual, server) pair, so both are checked once per XImage in
// adopt_image_layout() rather than re-derived on every presented frame.
// The pixel store below is a native uint32 write, while XImage::byte_order is the SERVER's, so on a
// big-endian server the bytes would land reversed with XPutImage performing no swap (the image
// already claims the server's order). MIT-SHM is local-only so that can only arise on the
// XPutImage fallback against a remote big-endian server; it is refused in ensure_image().
class X11ShmBlitter final : public IPresentBlitter
{
public:
    X11ShmBlitter(Display* display, Window window) : display_(display), window_(window)
    {
        const int screen = DefaultScreen(display_);
        visual_ = DefaultVisual(display_, screen);
        depth_ = static_cast<unsigned int>(DefaultDepth(display_, screen));
        gc_ = ::XCreateGC(display_, window_, 0, nullptr);
        shm_available_ = ::XShmQueryExtension(display_) != 0;
    }

    X11ShmBlitter(const X11ShmBlitter&) = delete;
    X11ShmBlitter& operator=(const X11ShmBlitter&) = delete;

    ~X11ShmBlitter() override
    {
        release_image();
        if (gc_ != nullptr)
        {
            ::XFreeGC(display_, gc_);
        }
    }

    [[nodiscard]] const char* name() const override
    {
        return using_shm_ ? "x11-shm" : "x11-putimage";
    }

    bool blit(const BlitImage& src, Extent2D dst) override;

private:
    [[nodiscard]] bool ensure_image(Extent2D size);
    // Every refusal that is a property of the (visual, server) pair rather than of one frame: the
    // pixel width, the channel field widths, and the byte order. Validated ONCE per XImage, in
    // ensure_image, instead of re-derived on every presented frame.
    [[nodiscard]] bool adopt_image_layout();
    void release_image();

    Display* display_ = nullptr;
    Window window_ = 0;
    Visual* visual_ = nullptr;
    unsigned int depth_ = 0;
    GC gc_ = nullptr;
    XImage* image_ = nullptr;
    XShmSegmentInfo shm_{};
    std::vector<std::uint8_t> fallback_pixels_;
    // The destination-x -> source-x map, rebuilt once per blit rather than per ROW. See blit().
    std::vector<std::uint32_t> column_map_;
    Extent2D image_size_{};
    int red_shift_ = 0;
    int green_shift_ = 0;
    int blue_shift_ = 0;
    bool shm_available_ = false;
    bool using_shm_ = false;
};

// XShmAttach fails ASYNCHRONOUSLY: the request is queued and the BadAccess arrives later, after the
// blitter has happily started using a segment the server never mapped. The only reliable probe is
// to install a handler, XSync, and read the flag — the canonical Xlib idiom, and the reason
// g_shm_attach_failed exists at file scope (Xlib's handler takes no user pointer).
bool g_shm_attach_failed = false;

int shm_attach_error_handler(Display* /*display*/, XErrorEvent* /*error*/)
{
    g_shm_attach_failed = true;
    return 0;
}

void X11ShmBlitter::release_image()
{
    if (image_ == nullptr)
    {
        return;
    }
    if (using_shm_)
    {
        ::XShmDetach(display_, &shm_);
        XDestroyImage(image_);
        if (shm_.shmaddr != nullptr)
        {
            ::shmdt(shm_.shmaddr);
        }
        shm_ = XShmSegmentInfo{};
    }
    else
    {
        // The fallback XImage does NOT own its data (it points into fallback_pixels_), so the buffer
        // is detached before XDestroyImage, which would otherwise free memory it never allocated.
        image_->data = nullptr;
        XDestroyImage(image_);
    }
    image_ = nullptr;
    using_shm_ = false;
    image_size_ = Extent2D{};
}

bool X11ShmBlitter::adopt_image_layout()
{
    // 32 bits per pixel is the only layout blit() writes. A 16-bit visual is a real X configuration,
    // just not one the editor presents into — refusing is honest rather than drawing garbage.
    if (image_->bits_per_pixel != 32)
    {
        return false;
    }

    const auto shift_of = [](unsigned long mask) {
        int shift = 0;
        while (mask != 0 && (mask & 1u) == 0)
        {
            mask >>= 1;
            ++shift;
        }
        return shift;
    };
    red_shift_ = shift_of(image_->red_mask);
    green_shift_ = shift_of(image_->green_mask);
    blue_shift_ = shift_of(image_->blue_mask);

    // Each channel FIELD must be exactly 8 bits wide, because the source is 8-bit BGRA and blit()
    // shifts a whole byte into place. 32 bits per pixel does NOT imply that: a depth-30 TrueColor
    // visual (common on HDR panels) is also 32 bpp, with 10-bit masks (0x3FF00000 / 0x000FFC00 /
    // 0x000003FF). An 8-bit sample shifted to bit 20 there occupies only the low 8 of the 10-bit
    // field, so every colour would come out roughly 4x too dark — silently, which is the exact class
    // of bug composing from the masks exists to rule out.
    if ((image_->red_mask >> red_shift_) != 0xFFuL ||
        (image_->green_mask >> green_shift_) != 0xFFuL ||
        (image_->blue_mask >> blue_shift_) != 0xFFuL)
    {
        return false;
    }

    // blit() stores pixels with a NATIVE uint32 write, but XImage::byte_order is the SERVER's. On a
    // mismatch the bytes land reversed and XPutImage performs no swap — the image already claims the
    // server's order — so the whole UI would present with red and blue exchanged.
    return image_->byte_order ==
           (std::endian::native == std::endian::little ? LSBFirst : MSBFirst);
}

bool X11ShmBlitter::ensure_image(Extent2D size)
{
    if (image_ != nullptr && image_size_.width == size.width && image_size_.height == size.height)
    {
        return true;
    }
    release_image();
    if (is_empty(size))
    {
        return false;
    }

    if (shm_available_)
    {
        image_ = ::XShmCreateImage(display_, visual_, depth_, ZPixmap, nullptr, &shm_, size.width,
                                   size.height);
        if (image_ != nullptr)
        {
            const std::size_t bytes =
                static_cast<std::size_t>(image_->bytes_per_line) * size.height;
            shm_.shmid = ::shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
            if (shm_.shmid >= 0)
            {
                shm_.shmaddr = static_cast<char*>(::shmat(shm_.shmid, nullptr, 0));
                // Marked for destruction IMMEDIATELY after attaching: the segment then disappears
                // once the last process detaches, so a crashed editor cannot leak kernel SHM
                // segments for the rest of the machine's uptime.
                ::shmctl(shm_.shmid, IPC_RMID, nullptr);
            }
            if (shm_.shmid >= 0 && shm_.shmaddr != reinterpret_cast<char*>(-1) &&
                shm_.shmaddr != nullptr)
            {
                image_->data = shm_.shmaddr;
                shm_.readOnly = 0;
                g_shm_attach_failed = false;
                XErrorHandler previous = ::XSetErrorHandler(&shm_attach_error_handler);
                const int attached = ::XShmAttach(display_, &shm_);
                ::XSync(display_, 0);
                ::XSetErrorHandler(previous);
                if (attached != 0 && !g_shm_attach_failed)
                {
                    if (!adopt_image_layout())
                    {
                        ::XShmDetach(display_, &shm_);
                        ::XSync(display_, 0);
                        ::shmdt(shm_.shmaddr);
                        XDestroyImage(image_);
                        image_ = nullptr;
                        shm_ = XShmSegmentInfo{};
                        return false;
                    }
                    using_shm_ = true;
                    image_size_ = size;
                    return true;
                }
                ::shmdt(shm_.shmaddr);
            }
            XDestroyImage(image_);
            image_ = nullptr;
            shm_ = XShmSegmentInfo{};
            // One failed attach means this display cannot share memory at all (it is remote, or the
            // container forbids SysV SHM). Remembering that avoids paying the XSync round-trip on
            // every resize for the rest of the session.
            shm_available_ = false;
        }
    }

    image_ = ::XCreateImage(display_, visual_, depth_, ZPixmap, 0, nullptr, size.width, size.height,
                            32, 0);
    if (image_ == nullptr)
    {
        return false;
    }
    if (!adopt_image_layout())
    {
        release_image();
        return false;
    }
    fallback_pixels_.assign(static_cast<std::size_t>(image_->bytes_per_line) * size.height, 0u);
    image_->data = reinterpret_cast<char*>(fallback_pixels_.data());
    image_size_ = size;
    return true;
}

bool X11ShmBlitter::blit(const BlitImage& src, Extent2D dst)
{
    const BlitPlan plan = compute_blit_plan(src.size, dst);
    if (plan.empty || !is_blit_source_readable(src) || gc_ == nullptr)
    {
        return false;
    }
    if (!ensure_image(dst))
    {
        return false;
    }
    // The pixel width, the channel field widths and the byte order were all validated once, against
    // this XImage, in ensure_image -> adopt_image_layout. Nothing about them can change per frame.

    // The destination-x -> source-x map. Each entry costs a 64-bit DIVISION, and it is invariant in
    // y — so computing it inside the pixel loop, as the obvious form does, pays plan.width * height
    // divides per presented frame (~3.7M at 2560x1440) on the one present path that exists precisely
    // BECAUSE the box has no GPU. A 64-bit divide is the slowest common integer instruction and a
    // loop-invariant divisor is not something the compiler can hoist here. Building the map once per
    // blit is the same arithmetic, height-fold fewer times; keeping it as a member also avoids
    // re-allocating it every frame.
    column_map_.resize(plan.width);
    for (std::uint32_t i = 0; i < plan.width; ++i)
    {
        column_map_[i] = blit_source_index(i, src.size.width, plan.width);
    }

    const auto plan_x = static_cast<std::uint32_t>(plan.x);
    const auto plan_y = static_cast<std::uint32_t>(plan.y);
    const auto* base = static_cast<const std::uint8_t*>(src.pixels);
    for (std::uint32_t y = 0; y < dst.height; ++y)
    {
        auto* row = reinterpret_cast<std::uint32_t*>(
            image_->data + static_cast<std::size_t>(y) * image_->bytes_per_line);
        if (y < plan_y || y >= plan_y + plan.height)
        {
            // The letterbox bars. Written every frame rather than once, because a resize changes
            // where they are and a stale bar leaves the previous frame's edge on screen.
            std::fill_n(row, dst.width, 0u);
            continue;
        }
        // Nearest-neighbour, top-left convention — the SAME arithmetic MemoryBlitter uses, now
        // literally the same function, so the portable oracle really does predict what lands on an
        // X11 window.
        const std::uint32_t sy = blit_source_index(y - plan_y, src.size.height, plan.height);
        const std::uint8_t* src_row = base + static_cast<std::size_t>(sy) * src.bytes_per_row;
        // The pillarbox bars, then the sampled span — split rather than branch-tested per pixel.
        std::fill_n(row, plan_x, 0u);
        std::fill_n(row + plan_x + plan.width, dst.width - plan_x - plan.width, 0u);
        for (std::uint32_t x = plan_x; x < plan_x + plan.width; ++x)
        {
            const std::uint32_t sx = column_map_[x - plan_x];
            const std::uint8_t* s = src_row + static_cast<std::size_t>(sx) * 4u;
            // Source is BGRA; the destination channel positions come from the visual's masks.
            row[x] = (static_cast<std::uint32_t>(s[2]) << red_shift_) |
                     (static_cast<std::uint32_t>(s[1]) << green_shift_) |
                     (static_cast<std::uint32_t>(s[0]) << blue_shift_);
        }
    }

    if (using_shm_)
    {
        // send_event False: the Shell does not wait for a completion event, and asking for one
        // without draining it fills the queue with ShmCompletion events nobody reads.
        (void)::XShmPutImage(display_, window_, gc_, image_, 0, 0, 0, 0, dst.width, dst.height, 0);
    }
    else
    {
        (void)::XPutImage(display_, window_, gc_, image_, 0, 0, 0, 0, dst.width, dst.height);
    }
    ::XFlush(display_);
    return true;
}

} // namespace
#endif // __linux__ && CONTEXT_PRESENT_HAS_X11

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

std::unique_ptr<IPresentBlitter> make_x11_shm_blitter(void* display, void* window)
{
#if defined(__linux__) && defined(CONTEXT_PRESENT_HAS_X11)
    if (display == nullptr || window == nullptr)
    {
        return nullptr;
    }
    // The XID travelled as a widened pointer (rhi.h's XlibWindow contract), so it is cast BACK
    // through uintptr_t — reinterpreting the pointer as a Window directly is a different, and on
    // some ABIs differently-sized, conversion.
    return std::make_unique<X11ShmBlitter>(
        static_cast<Display*>(display),
        static_cast<Window>(reinterpret_cast<std::uintptr_t>(window)));
#else
    (void)display;
    (void)window;
    return nullptr;
#endif
}

#if !defined(__APPLE__)
std::unique_ptr<IPresentBlitter> make_cocoa_layer_blitter(void* layer)
{
    // The real one lives in present_blit_mac.mm — Objective-C++ cannot be compiled into this TU, and
    // CMake picks a compiler by file extension, so the two halves are two files rather than one
    // `#if`/`#else` pair (the shape the GDI and X11 blitters take above). The refusal is still a
    // VALUE the ctest asserts on every leg, which is the property that mattered.
    (void)layer;
    return nullptr;
}
#endif // !__APPLE__

BlitterSelection make_present_blitter(const NativeWindowDesc& native)
{
    BlitterSelection selection;
    if (native.kind == NativeWindowKind::None)
    {
        // The honest report of the headless backend: there is no presentable native window at all,
        // which is a supported configuration and not a failure.
        selection.diagnostic = "no native window: nothing to present into";
        return selection;
    }
    if (native.handle == nullptr)
    {
        selection.diagnostic = "no native window handle: nothing to present into";
        return selection;
    }

    switch (native.kind)
    {
    case NativeWindowKind::None:
        break; // handled above; listed so a new kind cannot be added without visiting this switch
    case NativeWindowKind::Win32Hwnd:
        selection.blitter = make_win32_gdi_blitter(native.handle);
        if (selection.blitter == nullptr)
        {
            selection.diagnostic =
                "the GDI blitter is compiled only on Windows; this build cannot present on win32";
        }
        return selection;
    case NativeWindowKind::XlibWindow:
        if (native.display == nullptr)
        {
            selection.diagnostic =
                "no X display connection: an X11 present needs the Display* as well as the Window";
            return selection;
        }
        selection.blitter = make_x11_shm_blitter(native.display, native.handle);
        if (selection.blitter == nullptr)
        {
            selection.diagnostic = "the X11 blitter is compiled only on a Linux build configured "
                                   "with the X11 development headers; this build cannot present "
                                   "on X11";
        }
        return selection;
    case NativeWindowKind::WaylandSurface:
        // A NAMED refusal, which is the whole reason this selection is keyed on the window system
        // rather than on PresentPlatform: a Wayland surface shares `PresentPlatform::linux_` with
        // X11, so a platform-keyed switch would have handed it to the X11 blitter and crashed on a
        // wl_surface* reinterpreted as a Window.
        selection.diagnostic = "no Wayland present blitter in this build — the Shell targets "
                               "X11/XWayland on Linux (D21); a native wl_surface backend is "
                               "post-M9";
        return selection;
    case NativeWindowKind::MetalLayer:
        selection.blitter = make_cocoa_layer_blitter(native.handle);
        if (selection.blitter == nullptr)
        {
            selection.diagnostic = "the CALayer.contents blitter is compiled only on macOS; this "
                                   "build cannot present on a CAMetalLayer";
        }
        return selection;
    }
    selection.diagnostic = "unknown native window kind";
    return selection;
}

} // namespace context::render::present
