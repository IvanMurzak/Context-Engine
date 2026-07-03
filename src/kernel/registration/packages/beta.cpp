// DCE proof fixture: demo package "beta". Beta is the CONTROL — it is referenced by BOTH probe
// variants, so its marker must be present in both binaries (proving the mechanism does not simply
// strip every package). See alpha.cpp for the eliminated case (R-KERNEL-003).

#include "context/kernel/registration/registrar.h"

#include <cstdio>
#include <memory>
#include <string_view>

namespace context::kernel::registration
{
namespace
{
// Unique, greppable marker tied to beta's code path. Kept in sync with dce_footprint_check.cmake.
constexpr const char* kBetaMarker = "CONTEXT_DCE_MARKER_beta_7c1d4e_payload";

class BetaModule final : public Module
{
public:
    [[nodiscard]] std::string_view name() const override { return "beta"; }

    // The observable side effect that keeps the marker alive under LTO when beta IS referenced.
    void on_register(World& /*world*/, EventBus& /*events*/) override { std::puts(kBetaMarker); }
};
} // namespace

// Explicit registration entry point. Named by the generated registration TU of both probe variants.
void register_beta(Kernel& kernel) { kernel.add_module(std::make_unique<BetaModule>()); }

} // namespace context::kernel::registration
