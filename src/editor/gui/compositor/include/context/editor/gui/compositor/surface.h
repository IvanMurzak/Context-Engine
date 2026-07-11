// L-41 surface-handoff seam (R-UI-007 / L-41, issue #152): the minimal, headless model of the
// ratified per-platform CEF compositing tree. This is the SEAM the F1 viewport later fills — NOT
// full 3D compositing. It answers exactly one question deterministically: given a host platform and a
// probed GPU/windowing capability set, which CEF OSR mode does L-41 select, and what does the
// surface handoff look like?
//
// The ratified L-41 tree (DESIGN-DECISIONS.md L-41):
//   * Windows = accelerated OSR PRIMARY (shared-texture) with the software-OSR branch COMPILED-IN as
//     a safe fallback (used when no GPU compositor is present).
//   * macOS  = accelerated via raw IOSurface with CEF-INTERNAL frame pacing ONLY — never design
//     against SendExternalBeginFrame (broken upstream, cef#4033).
//   * Linux  = software-OSR is the SHIPPED DEFAULT; accelerated only behind a Mesa/X11-ozone
//     capability gate, and forced to software on NVIDIA proprietary.
//
// The selection logic takes an EXPLICIT platform argument so all three branches are exercised by the
// headless ctest on every OS (the local Windows dev gate would otherwise never compile/run the mac +
// linux branches — conventions.md § platform-#if blind spot). current_platform() is the only
// compile-time #if, and it is trivial.

#pragma once

namespace context::editor::gui::compositor
{

// The L-41 per-platform CEF compositing mode.
enum class CompositingMode
{
    accelerated_osr, // GPU shared-texture OSR (Windows primary; Linux only behind the capability gate)
    software_osr,    // CPU OSR readback (Linux default; the Windows compiled-in fallback)
    iosurface,       // macOS raw IOSurface, CEF-internal frame pacing
};

enum class HostPlatform
{
    windows,
    macos,
    linux_, // trailing underscore: `linux` can be a predefined macro on some toolchains
};

// The GPU/windowing capability probe the L-41 gate consults. On Linux, accelerated OSR is enabled
// ONLY when a Mesa/X11-ozone-capable stack is present AND it is not NVIDIA proprietary. On Windows,
// the accelerated path is primary but falls back to the compiled-in software branch when no GPU
// compositor is present. On macOS the probe is advisory (IOSurface is the fixed path).
struct SurfaceCapabilities
{
    bool gpu_compositing = true;    // a usable GPU compositor is present
    bool mesa_x11_ozone = false;    // Linux: the Mesa/X11-ozone stack the accel gate requires
    bool nvidia_proprietary = false; // Linux: forces software OSR regardless of the above
};

// A surface-handoff descriptor for the selected mode.
struct SurfaceHandoff
{
    CompositingMode mode;
    bool shared_texture;       // true for accelerated_osr / iosurface (zero-copy GPU handoff)
    bool external_begin_frame; // ALWAYS false — L-41 forbids designing against SendExternalBeginFrame
};

// Select the L-41 compositing mode for `platform` given the probed `caps`, encoding the ratified
// per-platform tree above. Deterministic + total (never asserts).
[[nodiscard]] CompositingMode select_mode(HostPlatform platform, const SurfaceCapabilities& caps);

// Build the surface-handoff descriptor for the selected mode. `shared_texture` is true for the
// accelerated / IOSurface modes and false for software OSR; `external_begin_frame` is ALWAYS false.
[[nodiscard]] SurfaceHandoff make_handoff(HostPlatform platform, const SurfaceCapabilities& caps);

// The current host platform (the only compile-time #if in this seam). Used by the CEF host; the
// headless tests pass an explicit platform so all three branches run on every OS.
[[nodiscard]] HostPlatform current_platform();

} // namespace context::editor::gui::compositor
