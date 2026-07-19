// macOS IOSurface -> Metal blit for the accelerated OSR import path — see metal_interop.h.
//
// Objective-C++, compiled ONLY on Apple (guarded in src/render/CMakeLists.txt AND by the __APPLE__
// test below, so an accidental inclusion elsewhere is an empty TU rather than a build break).

#include "metal_interop.h"

#if defined(__APPLE__)

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

namespace context::render::detail
{

std::string blit_iosurface(void* mtl_device, void* mtl_queue, void* iosurface,
                           void* dst_mtl_texture, std::uint32_t width, std::uint32_t height)
{
    if (mtl_device == nullptr)
    {
        return "wgpuDeviceGetNativeMetalDevice returned null (the active backend is not Metal)";
    }
    if (mtl_queue == nullptr)
    {
        return "wgpuQueueGetNativeMetalCommandQueue returned null";
    }
    if (dst_mtl_texture == nullptr)
    {
        return "wgpuTextureGetNativeMetalTexture returned null";
    }
    if (iosurface == nullptr)
    {
        return "the producer supplied no IOSurface for this frame";
    }
    if (width == 0 || height == 0)
    {
        return "the producer supplied an empty IOSurface extent";
    }

    @autoreleasepool
    {
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mtl_queue;
        id<MTLTexture> destination = (__bridge id<MTLTexture>)dst_mtl_texture;
        IOSurfaceRef surface = (IOSurfaceRef)iosurface;

        // Wrap the producer's IOSurface as a Metal texture. CEF delivers BGRA8 with premultiplied
        // alpha, which is what the composite pass expects, so the format matches the wgpu-side
        // BGRA8Unorm texture exactly and the blit is a straight copy — no conversion pass.
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:width
                                        height:height
                                     mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeManaged;

        id<MTLTexture> source = [device newTextureWithDescriptor:descriptor
                                                       iosurface:surface
                                                           plane:0];
        if (source == nil)
        {
            return "newTextureWithDescriptor:iosurface:plane: failed for the producer's IOSurface";
        }

        // Copy only the overlap: a paint can race a resize, so the producer's surface and our
        // imported texture may disagree on size for one frame. Clamping keeps that frame correct
        // instead of trapping in the Metal validation layer.
        const NSUInteger copy_width = MIN(source.width, destination.width);
        const NSUInteger copy_height = MIN(source.height, destination.height);
        if (copy_width == 0 || copy_height == 0)
        {
            return "the IOSurface and the imported texture do not overlap";
        }

        id<MTLCommandBuffer> commands = [queue commandBuffer];
        if (commands == nil)
        {
            return "could not obtain a Metal command buffer for the IOSurface blit";
        }
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (blit == nil)
        {
            return "could not obtain a Metal blit encoder for the IOSurface blit";
        }
        [blit copyFromTexture:source
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(copy_width, copy_height, 1)
                    toTexture:destination
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [commands commit];
        // Wait: the composite pass samples this texture in the SAME frame, and wgpu's queue knows
        // nothing about a command buffer we submitted behind its back, so there is no other
        // ordering guarantee available at this seam.
        [commands waitUntilCompleted];

        if (commands.error != nil)
        {
            return std::string("the IOSurface blit failed: ") +
                   [[commands.error localizedDescription] UTF8String];
        }
    }
    return {};
}

} // namespace context::render::detail

#endif // __APPLE__
