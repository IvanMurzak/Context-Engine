// L-41 surface-handoff seam tests: assert the ratified per-platform tree, exercising ALL THREE
// platform branches on every OS (explicit platform argument — see surface.h).

#include "context/editor/gui/compositor/surface.h"

#include "compositor_test.h"

using namespace context::editor::gui::compositor;

int main()
{
    // --- Windows: accelerated OSR primary; software fallback when no GPU compositor --------------
    {
        SurfaceCapabilities gpu;
        gpu.gpu_compositing = true;
        CHECK(select_mode(HostPlatform::windows, gpu) == CompositingMode::accelerated_osr);

        SurfaceCapabilities no_gpu;
        no_gpu.gpu_compositing = false;
        CHECK(select_mode(HostPlatform::windows, no_gpu) == CompositingMode::software_osr);

        const SurfaceHandoff h = make_handoff(HostPlatform::windows, gpu);
        CHECK(h.mode == CompositingMode::accelerated_osr);
        CHECK(h.shared_texture);
        CHECK(!h.external_begin_frame); // never SendExternalBeginFrame
    }

    // --- macOS: IOSurface, CEF-internal pacing (fixed; probe advisory) --------------------------
    {
        SurfaceCapabilities caps; // defaults
        CHECK(select_mode(HostPlatform::macos, caps) == CompositingMode::iosurface);
        // even with a "no GPU" probe macOS stays IOSurface (fixed path)
        SurfaceCapabilities no_gpu;
        no_gpu.gpu_compositing = false;
        CHECK(select_mode(HostPlatform::macos, no_gpu) == CompositingMode::iosurface);

        const SurfaceHandoff h = make_handoff(HostPlatform::macos, caps);
        CHECK(h.mode == CompositingMode::iosurface);
        CHECK(h.shared_texture);
        CHECK(!h.external_begin_frame);
    }

    // --- Linux: software default; accelerated ONLY behind the Mesa/X11-ozone gate ----------------
    {
        // default (no mesa/x11 ozone reported) -> software
        SurfaceCapabilities def;
        CHECK(select_mode(HostPlatform::linux_, def) == CompositingMode::software_osr);

        // Mesa/X11-ozone capable + GPU -> accelerated
        SurfaceCapabilities mesa;
        mesa.gpu_compositing = true;
        mesa.mesa_x11_ozone = true;
        mesa.nvidia_proprietary = false;
        CHECK(select_mode(HostPlatform::linux_, mesa) == CompositingMode::accelerated_osr);

        // NVIDIA proprietary forces software even when mesa/x11 ozone is reported
        SurfaceCapabilities nvidia;
        nvidia.gpu_compositing = true;
        nvidia.mesa_x11_ozone = true;
        nvidia.nvidia_proprietary = true;
        CHECK(select_mode(HostPlatform::linux_, nvidia) == CompositingMode::software_osr);

        // no GPU compositor -> software regardless of the gate flags
        SurfaceCapabilities no_gpu;
        no_gpu.gpu_compositing = false;
        no_gpu.mesa_x11_ozone = true;
        CHECK(select_mode(HostPlatform::linux_, no_gpu) == CompositingMode::software_osr);

        const SurfaceHandoff soft = make_handoff(HostPlatform::linux_, def);
        CHECK(soft.mode == CompositingMode::software_osr);
        CHECK(!soft.shared_texture); // software OSR = CPU readback, no zero-copy GPU handoff
        CHECK(!soft.external_begin_frame);
    }

    // --- current_platform() returns a value consistent with select_mode ------------------------
    {
        const HostPlatform here = current_platform();
        SurfaceCapabilities caps;
        // just assert it does not crash + returns SOME valid mode for the compiled platform
        const CompositingMode m = select_mode(here, caps);
        CHECK(m == CompositingMode::accelerated_osr || m == CompositingMode::software_osr ||
              m == CompositingMode::iosurface);
    }

    UITREE_TEST_MAIN_END();
}
