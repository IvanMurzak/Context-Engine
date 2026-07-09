// The T1 native RHI backend factory over wgpu-native (R-REND-001/002, L-56). Promotes the throwaway
// spikes/webgpu de-risk to a real subsystem: the SHA-pinned wgpu-native prebuilt (R-SEC-009 signed-
// prebuilt exception; #76 V8 Option-A precedent). This header pulls in NO wgpu/webgpu types — the
// whole backend hides behind the rhi.h abstraction, so only src/render/src/wgpu/*.cpp (built solely
// under CONTEXT_BUILD_RENDER_WGPU) ever touches webgpu.h.

#pragma once

#include "context/render/rhi.h"

#include <memory>

namespace context::render
{

// Create the wgpu-native T1 RHI, or nullptr when a WebGPU instance cannot be created (e.g. the
// platform has no WebGPU at all — a clean headless outcome, not a hard error).
std::unique_ptr<IRhi> create_wgpu_rhi();

} // namespace context::render
