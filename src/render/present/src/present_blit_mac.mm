// The macOS CPU present fallback (M9 e12b) — a CGImage assigned to CALayer.contents.
//
// The exact counterpart of the Windows GDI (StretchDIBits) and Linux X11-SHM implementations in
// present_blit.cpp: no GPU, no Metal device, no swapchain, which is exactly the point. It is a
// separate FILE rather than another `#if` arm in present_blit.cpp for the reason recorded at
// make_cocoa_layer_blitter's non-Apple half — CMake picks a compiler by file extension, so a single
// `.cpp` cannot hold Objective-C++ and a single `.mm` would have to be compiled as Objective-C++ on
// Windows and Linux too, where no OBJCXX compiler is enabled at all.
//
// WHY IT COMPOSES THROUGH MemoryBlitter. MemoryBlitter is the ORACLE the blit geometry is asserted
// against on all three OSes; running the real macOS present THROUGH it makes the letterbox/pillarbox
// arithmetic and the nearest-neighbour sampling pixel-identical to the tested path by CONSTRUCTION.
// That matters more here than on the two sibling platforms: until M9 e12c-3 no CI job ran a
// windowed macOS test at all, so a hand-written second copy of the scale would have had exactly
// zero coverage. The cost is one extra pass over a buffer on the path that exists only because
// there is no GPU at all.
//
// WHAT IS COVERED, AND WHAT IS STILL HONESTLY UNVERIFIED. `test_present_blit.cpp` reaches this file
// only through make_cocoa_layer_blitter's null-argument guard, so through e12b the whole body below
// carried NO runtime coverage. Since M9 e12c-3 the `editor-shell-cocoa-window` smoke RUNS it: on
// the macos-latest leg it attaches this blitter over a REAL NSWindow's layer (asserting the
// resolved name is `cocoa-calayer`, so no fallback can satisfy it) and then asserts
// frames_presented >= 1, a counter that advances only when the ATTACHED blitter's blit() returned
// true. So the CFDataCreate copy, the CGImage construction, the CATransaction suppression and the
// early returns are executed code now — on ONE leg. The GEOMETRY stays covered on all three OSes
// for the separate reason above: MemoryBlitter is exercised directly by that suite, and this class
// composes through it. STILL UNVERIFIED: that the presented frame is VISIBLE ON SCREEN. A blit()
// that returned true with the wrong kCGImageAlphaPremultipliedLast byte order, or that the window
// server never composited, satisfies every assertion above — nothing reads pixels back OUT of the
// window server, which would need a screen capture no hosted runner offers. Stated here rather than
// implied, so the next reader does not mistake "blit() runs" for "the frame is on screen".

#include "context/render/present/present_blit.h"

#if defined(__APPLE__)

#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace context::render::present
{
namespace
{

class CocoaLayerBlitter final : public IPresentBlitter
{
public:
    explicit CocoaLayerBlitter(CALayer* layer) : layer_(layer) {}

    CocoaLayerBlitter(const CocoaLayerBlitter&) = delete;
    CocoaLayerBlitter& operator=(const CocoaLayerBlitter&) = delete;

    [[nodiscard]] const char* name() const override { return "cocoa-calayer"; }

    bool blit(const BlitImage& src, Extent2D dst) override
    {
        if (layer_ == nil)
        {
            return false;
        }
        // MemoryBlitter applies the plan (aspect fit, centred, bars zeroed) and the nearest-neighbour
        // scale into a dst-sized RGBA8 buffer, and refuses an unreadable source — so every geometry
        // decision on this path is the one the cross-platform tests already pin.
        if (!memory_.blit(src, dst))
        {
            return false;
        }
        const std::vector<std::uint8_t>& pixels = memory_.target();
        const Extent2D size = memory_.target_size();
        if (pixels.empty() || size.width == 0 || size.height == 0)
        {
            return false;
        }

        @autoreleasepool
        {
            // CFDataCreate COPIES. CGDataProviderCreateWithData over `pixels.data()` would not, and
            // the CGImage handed to the layer outlives this call — the very next present resizes or
            // rewrites that vector, so a non-copying provider would leave the layer displaying freed
            // memory. One copy per present is the correct price on the path that exists only because
            // the host has no GPU.
            CFDataRef data = ::CFDataCreate(kCFAllocatorDefault, pixels.data(),
                                            static_cast<CFIndex>(pixels.size()));
            if (data == nullptr)
            {
                return false;
            }
            CGDataProviderRef provider = ::CGDataProviderCreateWithCFData(data);
            CFRelease(data);
            if (provider == nullptr)
            {
                return false;
            }
            CGColorSpaceRef space = ::CGColorSpaceCreateDeviceRGB();
            if (space == nullptr)
            {
                ::CGDataProviderRelease(provider);
                return false;
            }
            // MemoryBlitter writes R,G,B,A in that byte order (it swizzles the BGRA source), which
            // is kCGImageAlphaPremultipliedLast under the DEFAULT byte order. Naming
            // kCGBitmapByteOrder32Big instead would be right on a big-endian host and this is not
            // one; naming ...32Little would swap red and blue on the whole UI.
            const CGBitmapInfo info =
                static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrderDefault;
            CGImageRef image = ::CGImageCreate(
                static_cast<size_t>(size.width), static_cast<size_t>(size.height), 8, 32,
                static_cast<size_t>(size.width) * 4u, space, info, provider, nullptr, false,
                kCGRenderingIntentDefault);
            ::CGColorSpaceRelease(space);
            ::CGDataProviderRelease(provider);
            if (image == nullptr)
            {
                return false;
            }

            // Implicit animations OFF. A CALayer animates every `contents` change by default, so
            // without this each presented frame cross-fades into the last one — which reads as a
            // laggy, ghosting UI rather than as an animation nobody asked for.
            //
            // Message syntax, not `layer_.contents = ...`: inside a C++ member function the dot on a
            // MEMBER that happens to be an Objective-C object pointer is the one place Obj-C++
            // property syntax and C++ member access read alike, and this file gets exactly one
            // compile attempt per CI round.
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            [layer_ setContents:(__bridge id)image];
            [CATransaction commit];
            ::CGImageRelease(image);
        }
        return true;
    }

private:
    CALayer* layer_ = nil;
    MemoryBlitter memory_;
};

} // namespace

std::unique_ptr<IPresentBlitter> make_cocoa_layer_blitter(void* layer)
{
    if (layer == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<CocoaLayerBlitter>((__bridge CALayer*)layer);
}

} // namespace context::render::present

#endif // __APPLE__
