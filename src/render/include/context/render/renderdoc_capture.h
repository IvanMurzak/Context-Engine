// The L-47 RenderDoc (GPU) capture hook (a15, R-OBS-002). RenderDoc is a world-class GPU debugger;
// L-47 says "export to it, don't rebuild it." This is the in-application hook: when a developer
// launches the engine THROUGH RenderDoc (which injects renderdoc.dll / librenderdoc.so into the
// process), this seam resolves RenderDoc's in-app API and lets the render backend bracket a frame's
// GPU submit with StartFrameCapture / EndFrameCapture (or queue a one-shot TriggerCapture). When
// RenderDoc is NOT attached — the normal case, and always in CI/headless — every call is a safe
// no-op: the seam never LOADS RenderDoc, it only binds to an already-injected module.
//
// GPU-free by construction: this is pure module-handle + function-pointer plumbing (no wgpu, no GPU
// device), so it lives in the GPU-free context_render abstraction and builds + unit-tests under
// every toolchain (the not-attached path). The actual StartFrameCapture wrapping of a wgpu submit
// runs on the GPU leg — see docs/profiling.md § RenderDoc for the wiring.

#pragma once

#include <cstdint>

namespace context::render
{

// Binds (once) to an already-injected RenderDoc module and drives its in-app frame-capture API.
// Targets the stable RENDERDOC_API_1_1_2 ABI (the version RenderDoc has shipped since 2018); the
// four entry points used (GetAPIVersion / TriggerCapture / StartFrameCapture / EndFrameCapture) are
// at fixed, append-only offsets in every later API version, so a newer RenderDoc binds fine too.
class RenderDocCapture
{
public:
    RenderDocCapture() = default;

    // Resolve RENDERDOC_GetAPI from the injected module and obtain the API handle. Returns true when
    // RenderDoc is attached (idempotent — a second call is a no-op once bound). Never loads RenderDoc
    // itself: an absent module simply leaves the seam un-attached.
    bool load();

    [[nodiscard]] bool attached() const noexcept { return api_ != nullptr; }

    // The resolved RenderDoc API version, or all-zero when not attached. (Out-params are named
    // out_* rather than major/minor to dodge the glibc <sys/sysmacros.h> `major`/`minor` macros — a
    // Linux-only identifier collision the local Windows gate cannot see.)
    void api_version(int& out_major, int& out_minor, int& out_patch) const noexcept;

    // Bracket a frame's GPU work — the render backend calls begin before recording the frame's
    // command submit and end after it, so RenderDoc captures exactly that frame. NULL device/window
    // means "the active device, all windows" (RenderDoc's documented capture-everything default).
    // No-ops when not attached.
    void begin_frame_capture() noexcept;
    void end_frame_capture() noexcept;

    // Queue a capture of the NEXT frame that begins+ends (the F12-key equivalent, driven
    // programmatically). No-op when not attached.
    void trigger_capture() noexcept;

private:
    // The minimal prefix of the RENDERDOC_API_1_1_2 function-pointer table. Unused slots are pointer
    // padding at their exact ABI offsets (every member is a function pointer, so a void* placeholder
    // preserves the layout); only the four entry points this seam calls carry real signatures. The
    // slot indices are the canonical, append-only RenderDoc API order.
    struct Api
    {
        void (*GetAPIVersion)(int* out_major, int* out_minor, int* out_patch); // slot 0
        void* reserved_1_to_14[14];                                // slots 1..14
        void (*TriggerCapture)();                                  // slot 15
        void* reserved_16_to_18[3];                                // slots 16..18
        void (*StartFrameCapture)(void* device, void* window);     // slot 19
        void* reserved_20;                                         // slot 20 (IsFrameCapturing)
        std::uint32_t (*EndFrameCapture)(void* device, void* window); // slot 21
    };

    Api* api_ = nullptr;
};

} // namespace context::render
