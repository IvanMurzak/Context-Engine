// Both in-repo providers pass the SAME reusable conformance suite (M7 a11 DoD-1, R-UI-002): the headless
// NullProvider (context_ui) AND the engine-integrated GpuUiProvider (context_render_ui, driven over the
// GPU-free fake RHI backend) each satisfy run_provider_conformance() with zero failures. This is the
// R-UI-008 on-ramp exercised in-tree — the exact three-line shape an out-of-tree provider author reuses
// (construct the provider, call run_provider_conformance, assert 0). The GPU provider needs a device, so
// it rides the fake RHI (render_test_rhi.h) exactly like src/render/ui/tests/test_ui_provider.cpp.

#include "context/packages/ui/null_provider.h"
#include "context/render/ui/provider.h"

#include "provider_conformance.h"
#include "ui_test.h"

#include "render_test_rhi.h"

#include <memory>

using namespace context::packages::ui;

int main()
{
    // --- the null / headless provider: all render caps false, shaping/bidi headless-true (a8) --------
    {
        NullProvider provider;
        CHECK(conformance::run_provider_conformance(provider, "null") == 0);
    }

    // --- the engine-integrated GPU provider, driven end-to-end over the GPU-free fake RHI backend -----
    {
        rendertest::FakeRhi rhi(/*adapter_count=*/1);
        std::unique_ptr<context::render::IDevice> device = rhi.create_device();
        CHECK(device != nullptr);
        context::render::ui::GpuUiProvider provider(*device, context::render::Extent2D{128, 32},
                                                    context::render::Color{0.06, 0.07, 0.10, 1.0});
        CHECK(conformance::run_provider_conformance(provider, "engine-integrated") == 0);
    }

    UI_TEST_MAIN_END();
}
