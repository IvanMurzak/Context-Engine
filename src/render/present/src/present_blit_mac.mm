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
// That matters more here than on the two sibling platforms: no CI job runs a windowed macOS test, so
// a hand-written second copy of the scale would have exactly zero coverage. The cost is one extra
// pass over a buffer on the path that exists only because there is no GPU at all.
//
// WHAT IS HONESTLY UNVERIFIED — and it is MORE than the visible frame. No test on any leg calls
// CocoaLayerBlitter::blit() at all. `test_present_blit.cpp` reaches this file only through
// make_cocoa_layer_blitter's null-argument guard, so the whole body below — the CFDataCreate copy,
// the CGImage construction, the kCGImageAlphaPremultipliedLast byte-order choice, the CATransaction
// implicit-animation suppression, and all five early returns — carries NO runtime coverage. What IS
// covered, on all three OSes, is the geometry: MemoryBlitter is exercised directly by that same
// suite, and this class composes through it rather than re-deriving it, so the plan applied here is
// the tested one by construction. Closing the rest needs an Objective-C++ test that builds a real
// CALayer (Core Animation needs no Aqua session), plus the windowed proof that a presented frame is
// VISIBLE — both e12c's, since `editor-cef-smoke (macos-latest)` deliberately runs no windowed
// smoke. Stated here rather than implied, so the next reader does not mistake the geometry coverage
// above for coverage of this file.

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
