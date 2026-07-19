// macOS IOSurface -> Metal interop for the accelerated OSR import path (design 03 §3).
//
// Everything here is an OPAQUE void* on purpose: the declarations must be includable from
// wgpu_rhi.cpp, which is plain C++ and must never see <Metal/Metal.h>. The implementation lives in
// metal_interop.mm (Objective-C++), compiled ONLY on Apple platforms.
//
// Why this file exists at all: wgpu-native's stock C API (v29.0.1.1) exposes the three native Metal
// ACCESSORS added upstream in PR #557 — wgpuDeviceGetNativeMetalDevice,
// wgpuQueueGetNativeMetalCommandQueue and wgpuTextureGetNativeMetalTexture. They hand back the
// id<MTL*> objects behind wgpu's own handles, which is exactly enough to wrap the producer's
// IOSurface as an MTLTexture and blit it into a wgpu texture — with NO fork of wgpu-native. (An
// external-texture IMPORT entry point, which Windows would need, is a different thing and does not
// exist in the C API; see gfx-rs/wgpu-native#621.)

#pragma once

#include <cstdint>
#include <string>

namespace context::render::detail
{

// Blit `iosurface` into the MTLTexture backing a wgpu texture. All handles are the opaque pointers
// the wgpu native accessors return (`mtl_device` = id<MTLDevice>, `mtl_queue` = id<MTLCommandQueue>,
// `dst_mtl_texture` = id<MTLTexture>); `iosurface` is an IOSurfaceRef.
//
// Returns an EMPTY string on success, otherwise the reason it failed — the caller turns that into a
// fail-closed ExternalTexture rather than presenting a stale or blank frame.
[[nodiscard]] std::string blit_iosurface(void* mtl_device, void* mtl_queue, void* iosurface,
                                         void* dst_mtl_texture, std::uint32_t width,
                                         std::uint32_t height);

} // namespace context::render::detail
