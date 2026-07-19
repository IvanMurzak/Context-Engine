// The CPU present fallback (review C-F2; design 03 §2, §7) — how the editor still shows its UI on a
// host with NO usable GPU adapter, or after an unrecoverable device loss.
//
// The promise this mechanizes: "the editor UI never REQUIRES a GPU". When AdapterProbe::can_present
// is false, there is no swapchain to acquire and no composite pass to run — the Shell instead blits
// the software-OSR buffer to the window through an OS 2D primitive (GDI StretchDIBits on Windows,
// X11 SHM on Linux, CALayer.contents on macOS), and viewport panels draw their diagnostic
// placeholder (02 §6).
//
// This header lands the SEAM plus the WINDOWS implementation; the X11 and macOS implementations are
// e12's. That gap is deliberate and REPORTED, never silent: make_present_blitter returns a
// selection carrying an explicit diagnostic for a platform whose blitter does not exist yet, so a
// caller degrades loudly instead of quietly presenting nothing.
//
// The geometry (compute_blit_plan) is pure integer math kept apart from every OS call, which is what
// lets the letterbox/pillarbox arithmetic be pixel-asserted on all three OSes — including through
// MemoryBlitter, a portable blitter that runs the SAME plan into a buffer.

#pragma once

#include "context/render/present/osr_import.h"
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
    Extent2D size;
    std::uint32_t bytes_per_row = 0;
};

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

// What make_present_blitter resolved to, and — when it resolved to nothing — why.
struct BlitterSelection
{
    std::unique_ptr<IPresentBlitter> blitter;
    std::string diagnostic;
};

// Resolve the OS blitter for `platform`. `native_handle` is the platform's window handle (HWND on
// Windows). A platform with no implementation yet yields a null blitter plus a diagnostic naming it.
[[nodiscard]] BlitterSelection make_present_blitter(PresentPlatform platform, void* native_handle);

} // namespace context::render::present
