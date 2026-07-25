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

#if defined(__linux__) && defined(CONTEXT_PRESENT_HAS_X11)
namespace
{

// X11 present: scale the BGRA buffer into an XImage and push it to the window, through MIT-SHM when
// the server shares memory with us and plain XPutImage when it does not. Like the GDI path this
// needs no GPU, no EGL/GLX context and no swapchain, which is exactly the point.
//
// WHY THE PIXELS ARE COMPOSED FROM THE VISUAL'S MASKS rather than memcpy'd. A local 24/32-bit
// TrueColor visual on a little-endian box really is BGRX in memory, so a straight copy is right
// almost everywhere — and silently wrong on the rest (a big-endian server, or an RGBA visual),
// where it swaps red and blue on the whole UI. The per-pixel path costs one shift per channel on a
// path that already exists only because there is no GPU, and it cannot be wrong.
class X11ShmBlitter final : public IPresentBlitter
{
public:
    X11ShmBlitter(Display* display, Window window) : display_(display), window_(window)
    {
        screen_ = DefaultScreen(display_);
        visual_ = DefaultVisual(display_, screen_);
        depth_ = static_cast<unsigned int>(DefaultDepth(display_, screen_));
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
    void release_image();

    Display* display_ = nullptr;
    Window window_ = 0;
    int screen_ = 0;
    Visual* visual_ = nullptr;
    unsigned int depth_ = 0;
    GC gc_ = nullptr;
    XImage* image_ = nullptr;
    XShmSegmentInfo shm_{};
    std::vector<std::uint8_t> fallback_pixels_;
    Extent2D image_size_{};
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
    // 32 bits per pixel is the only layout this path writes. A 16-bit visual is a real X
    // configuration, just not one the editor supports presenting into — refusing is honest, and the
    // Shell reports the refusal rather than drawing garbage.
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
    const int red_shift = shift_of(image_->red_mask);
    const int green_shift = shift_of(image_->green_mask);
    const int blue_shift = shift_of(image_->blue_mask);

    const auto* base = static_cast<const std::uint8_t*>(src.pixels);
    for (std::uint32_t y = 0; y < dst.height; ++y)
    {
        auto* row = reinterpret_cast<std::uint32_t*>(
            image_->data + static_cast<std::size_t>(y) * image_->bytes_per_line);
        const bool inside_rows = y >= static_cast<std::uint32_t>(plan.y) &&
                                 y < static_cast<std::uint32_t>(plan.y) + plan.height;
        if (!inside_rows)
        {
            // The letterbox bars. Written every frame rather than once, because a resize changes
            // where they are and a stale bar leaves the previous frame's edge on screen.
            for (std::uint32_t x = 0; x < dst.width; ++x)
            {
                row[x] = 0u;
            }
            continue;
        }
        // Nearest-neighbour, top-left convention — the SAME arithmetic MemoryBlitter uses, so the
        // portable oracle really does predict what lands on an X11 window.
        const std::uint32_t sy = std::min(
            src.size.height - 1u,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(y - static_cast<std::uint32_t>(plan.y)) *
                 src.size.height) /
                plan.height));
        const std::uint8_t* src_row = base + static_cast<std::size_t>(sy) * src.bytes_per_row;
        for (std::uint32_t x = 0; x < dst.width; ++x)
        {
            if (x < static_cast<std::uint32_t>(plan.x) ||
                x >= static_cast<std::uint32_t>(plan.x) + plan.width)
            {
                row[x] = 0u;
                continue;
            }
            const std::uint32_t sx = std::min(
                src.size.width - 1u,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(x - static_cast<std::uint32_t>(plan.x)) *
                     src.size.width) /
                    plan.width));
            const std::uint8_t* s = src_row + static_cast<std::size_t>(sx) * 4u;
            // Source is BGRA; the destination channel positions come from the visual's masks.
            row[x] = (static_cast<std::uint32_t>(s[2]) << red_shift) |
                     (static_cast<std::uint32_t>(s[1]) << green_shift) |
                     (static_cast<std::uint32_t>(s[0]) << blue_shift);
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
        selection.diagnostic = "no CALayer.contents present blitter in this build — the macOS CPU "
                               "present fallback lands in e12b";
        return selection;
    }
    selection.diagnostic = "unknown native window kind";
    return selection;
}

} // namespace context::render::present
