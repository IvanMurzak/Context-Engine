// The CPU present fallback (review C-F2; design 03 §2, §7) — how the editor still shows its UI on a
// host with NO usable GPU adapter, or after an unrecoverable device loss.
//
// The promise this mechanizes: "the editor UI never REQUIRES a GPU". When AdapterProbe::can_present
// is false, there is no swapchain to acquire and no composite pass to run — the Shell instead blits
// the software-OSR buffer to the window through an OS 2D primitive (GDI StretchDIBits on Windows,
// X11 SHM on Linux, CALayer.contents on macOS), and viewport panels draw their diagnostic
// placeholder (02 §6).
//
// e03 landed the SEAM plus the WINDOWS implementation; e12a added the LINUX one (X11 MIT-SHM, with a
// plain XPutImage fallback for a display that cannot share memory); e12b adds the macOS one
// (CALayer.contents). All three v1 window systems now have a real CPU blitter, so
// make_present_blitter's remaining diagnostics are about a window system this build genuinely has no
// implementation for — Wayland, or a handle that arrived without its display — never about a
// platform nobody has written yet. That reporting is deliberate: a caller degrades loudly instead of
// quietly presenting nothing.
//
// The geometry (compute_blit_plan) is pure integer math kept apart from every OS call, which is what
// lets the letterbox/pillarbox arithmetic be pixel-asserted on all three OSes — including through
// MemoryBlitter, a portable blitter that runs the SAME plan into a buffer.

#pragma once

// present_common.h, NOT osr_import.h: the CPU present fallback needs the platform enum and the
// buffer-bounds rule, and nothing whatsoever from the OSR import policy.
#include "context/render/present/present_common.h"
#include "context/render/rhi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace context::render::present
{

// A CPU image to present: BGRA8, TOP-DOWN rows (CEF's OnPaint layout).
struct BlitImage
{
    const void* pixels = nullptr;
    // How many bytes `pixels` actually points at. Carried for the same reason OsrFrame::byte_size
    // is: a blitter reads height*bytes_per_row bytes on trust, so without it a truncated buffer is
    // an out-of-bounds read instead of a refusal.
    std::size_t byte_size = 0;
    Extent2D size;
    std::uint32_t bytes_per_row = 0;
};

// True when `src` describes a fully readable BGRA8 image: a real buffer, a stride at least as wide
// as its own pixels, and a byte_size covering through the last row. The ONE validation both
// blitters share, so neither can drift into trusting a malformed frame.
[[nodiscard]] bool is_blit_source_readable(const BlitImage& src);

// Copy `src` into a tightly-packed (bytes_per_row == width*4) BGRA8 buffer. A padded stride cannot
// be expressed in a Windows BITMAPINFO, so the GDI path repacks first — and because this is pure
// memory arithmetic rather than an OS call, it is asserted on all three OSes instead of only where
// GDI exists.
//
// `_into` is the form the per-frame path uses: it reuses `out`'s capacity, so a repacking blitter
// allocates once rather than once per paint. Returns false (and clears `out`) when `src` is not
// readable. The by-value overload is the convenience form for tests and one-shot callers.
bool repack_tight_into(const BlitImage& src, std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> repack_tight(const BlitImage& src);

// Where the source lands inside the destination surface: aspect-preserving and centred, so a window
// whose aspect differs from the UI buffer's gets symmetric bars rather than a stretched image.
struct BlitPlan
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // True when the fit left bars on one axis (letterbox horizontally / pillarbox vertically).
    bool letterboxed = false;
    // True when nothing can be drawn (a zero-sized source or destination — e.g. a minimized window).
    bool empty = true;
};

[[nodiscard]] BlitPlan compute_blit_plan(Extent2D src, Extent2D dst);

// The nearest-neighbour source index for one axis: floor(offset * src_extent / plan_extent), clamped
// to the last source texel. `offset_in_plan` is measured from the plan's origin, so a caller
// iterating DESTINATION space passes `x - plan.x` while one iterating plan space passes `x`.
//
// Shared deliberately, alongside is_blit_source_readable above. MemoryBlitter is the ORACLE the blit
// geometry is asserted against on every OS, and X11ShmBlitter cannot be unit-tested at all
// (constructing it issues X requests), so "the X11 path samples exactly what the oracle predicts"
// was a hand-maintained duplicate of a formula that no test could ever catch drifting. One function
// makes it structural instead of a comment.
[[nodiscard]] std::uint32_t blit_source_index(std::uint32_t offset_in_plan, std::uint32_t src_extent,
                                              std::uint32_t plan_extent);

// An OS-level 2D presentation primitive.
class IPresentBlitter
{
public:
    virtual ~IPresentBlitter() = default;

    [[nodiscard]] virtual const char* name() const = 0;

    // Present `src` scaled into a `dst`-sized surface. Returns false when there is nothing to draw
    // or the OS call failed; a false is reportable, never fatal.
    virtual bool blit(const BlitImage& src, Extent2D dst) = 0;
};

// The portable in-memory blitter: applies the SAME plan and nearest-neighbour scale into an RGBA8
// target. It is the oracle the blit geometry is asserted against on every OS, and doubles as the
// honest present target for a headless/offscreen shell.
class MemoryBlitter final : public IPresentBlitter
{
public:
    [[nodiscard]] const char* name() const override { return "memory"; }
    bool blit(const BlitImage& src, Extent2D dst) override;

    // The composed surface (RGBA8, target_size()), with untouched bar regions left at zero.
    [[nodiscard]] const std::vector<std::uint8_t>& target() const { return target_; }
    [[nodiscard]] Extent2D target_size() const { return target_size_; }
    [[nodiscard]] const BlitPlan& last_plan() const { return last_plan_; }
    [[nodiscard]] int blit_count() const { return blit_count_; }

private:
    std::vector<std::uint8_t> target_;
    Extent2D target_size_;
    BlitPlan last_plan_;
    int blit_count_ = 0;
};

// The Windows GDI blitter (StretchDIBits into the window's DC). Returns nullptr when `hwnd` is null
// or this translation unit was not built for Windows.
[[nodiscard]] std::unique_ptr<IPresentBlitter> make_win32_gdi_blitter(void* hwnd);

// The Linux X11 blitter (MIT-SHM XShmPutImage, degrading to XPutImage where shared memory is not
// available — a remote display, or a server built without the extension). `display` is the Display*
// and `window` is the Window XID widened to a pointer, matching rhi.h's XlibWindow contract.
// Returns nullptr when either is null, or when this translation unit was not built for Linux WITH
// the X11 development headers.
[[nodiscard]] std::unique_ptr<IPresentBlitter> make_x11_shm_blitter(void* display, void* window);

// The macOS blitter (a CGImage assigned to CALayer.contents). `layer` is the CAMetalLayer backing
// the Shell's NSView, matching rhi.h's MetalLayer contract — it is a CALayer, and `contents` is a
// plain CALayer property, so the CPU fallback needs nothing from Metal itself. Returns nullptr when
// `layer` is null, or when this build is not for macOS.
//
// ⚠ `layer` MUST be null or a REAL CALayer — there is no third case. The implementation holds it in
// an ARC-strong member, so a non-null argument is RETAINED on the way in: objc_retain dereferences
// whatever it is handed, and a pointer that is not an Objective-C object crashes HERE, not later at
// blit(). This is the one place the three platforms genuinely differ — Win32 validates handles, so
// make_win32_gdi_blitter above survives a junk HWND and reports the failure as a runtime false —
// and it is why the cross-platform test asserts only the null guard on Apple.
//
// It composes through MemoryBlitter rather than re-deriving the scale: that is the ORACLE the blit
// geometry is asserted against on every OS (see blit_source_index above), so the macOS path is
// pixel-identical to the tested one by CONSTRUCTION rather than by a comment — which matters more
// here than for the two siblings, because until M9 e12c-3 no CI leg ran a windowed macOS test at
// all.
[[nodiscard]] std::unique_ptr<IPresentBlitter> make_cocoa_layer_blitter(void* layer);

// What make_present_blitter resolved to, and — when it resolved to nothing — why.
struct BlitterSelection
{
    std::unique_ptr<IPresentBlitter> blitter;
    std::string diagnostic;
};

// Resolve the OS blitter for a native window. A window system with no implementation yet yields a
// null blitter plus a diagnostic naming it.
//
// KEYED ON NativeWindowDesc, NOT on PresentPlatform — e12a acting on the follow-up e03 recorded in
// docs/present-path.md. `PresentPlatform` is right for the IMPORT tier, which genuinely is
// OS-granular (DXGI / IOSurface / dmabuf), but a 2D present primitive is WINDOW-SYSTEM-granular:
// X11 SHM needs BOTH a `Display*` and a `Window`, and Wayland is an entirely different mechanism on
// the same `PresentPlatform::linux_`. `NativeWindowDesc` already carries exactly that shape
// (`kind` + `handle` + `display`, with X11 and Wayland as distinct kinds), so keying on it makes the
// Wayland case a NAMED refusal instead of a silent mis-dispatch into the X11 blitter — which is what
// bolting a second `void*` onto the platform-keyed signature would have produced.
[[nodiscard]] BlitterSelection make_present_blitter(const NativeWindowDesc& native);

} // namespace context::render::present
