// L-41 surface-handoff seam implementation: the ratified per-platform mode selection.

#include "context/editor/gui/compositor/surface.h"

namespace context::editor::gui::compositor
{

CompositingMode select_mode(HostPlatform platform, const SurfaceCapabilities& caps)
{
    switch (platform)
    {
    case HostPlatform::windows:
        // Accelerated OSR primary; the software branch is compiled-in and taken when there is no GPU
        // compositor (a genuinely-safe fallback — L-41).
        return caps.gpu_compositing ? CompositingMode::accelerated_osr
                                    : CompositingMode::software_osr;
    case HostPlatform::macos:
        // Raw IOSurface, CEF-internal pacing. Fixed path (the probe is advisory here).
        return CompositingMode::iosurface;
    case HostPlatform::linux_:
        // Software OSR is the shipped default; accelerated ONLY behind the Mesa/X11-ozone gate, and
        // NVIDIA proprietary forces software regardless.
        if (caps.gpu_compositing && caps.mesa_x11_ozone && !caps.nvidia_proprietary)
        {
            return CompositingMode::accelerated_osr;
        }
        return CompositingMode::software_osr;
    }
    return CompositingMode::software_osr;
}

SurfaceHandoff make_handoff(HostPlatform platform, const SurfaceCapabilities& caps)
{
    const CompositingMode mode = select_mode(platform, caps);
    SurfaceHandoff handoff{};
    handoff.mode = mode;
    handoff.shared_texture =
        (mode == CompositingMode::accelerated_osr) || (mode == CompositingMode::iosurface);
    handoff.external_begin_frame = false; // L-41: never SendExternalBeginFrame (cef#4033)
    return handoff;
}

HostPlatform current_platform()
{
#if defined(_WIN32)
    return HostPlatform::windows;
#elif defined(__APPLE__)
    return HostPlatform::macos;
#else
    return HostPlatform::linux_;
#endif
}

} // namespace context::editor::gui::compositor
