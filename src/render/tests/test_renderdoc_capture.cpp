// R-QA-013 test for the L-47 RenderDoc capture hook (renderdoc_capture.h) — the NOT-ATTACHED path,
// the only path reachable off a RenderDoc-injected launch (so it is what CI + the local gate can
// assert). RenderDoc is not injected here, so load() must fail cleanly and every capture call must
// be a safe no-op with attached() staying false and api_version() reporting all-zero. The attached
// path (real StartFrameCapture wrapping) runs only under a RenderDoc launch on the GPU leg — see
// docs/profiling.md § RenderDoc. GPU-free: pure module/function-pointer plumbing, a LOCAL gate on
// every toolchain.

#include "context/render/renderdoc_capture.h"

#include "render_test.h"

using context::render::RenderDocCapture;

int main()
{
    RenderDocCapture cap;

    // Not launched under RenderDoc: nothing is injected, so the seam binds to nothing.
    CHECK(!cap.attached());
    CHECK(!cap.load()); // no renderdoc.dll / librenderdoc.so in this process
    CHECK(!cap.attached());

    // api_version reports all-zero when not attached (and does not touch a null api).
    int out_major = -1;
    int out_minor = -1;
    int out_patch = -1;
    cap.api_version(out_major, out_minor, out_patch);
    CHECK(out_major == 0);
    CHECK(out_minor == 0);
    CHECK(out_patch == 0);

    // Every capture entry point is a safe no-op when not attached (no crash, no state change).
    cap.begin_frame_capture();
    cap.end_frame_capture();
    cap.trigger_capture();
    cap.begin_frame_capture();
    cap.end_frame_capture();
    CHECK(!cap.attached());

    // A second load() is still a clean false (idempotent, no partial state).
    CHECK(!cap.load());
    CHECK(!cap.attached());

    RENDER_TEST_MAIN_END();
}
