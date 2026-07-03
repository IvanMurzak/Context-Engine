// DCE proof fixture: demo package "alpha". Its unique marker is reachable only through
// register_alpha(); a build that does not reference alpha must leave zero trace of it (R-KERNEL-003).

#include "context/kernel/registration/registrar.h"

#include <cstdio>
#include <memory>
#include <string_view>

namespace context::kernel::registration
{
namespace
{
// Unique, greppable marker tied to alpha's code path. Kept in sync with dce_footprint_check.cmake.
constexpr const char* kAlphaMarker = "CONTEXT_DCE_MARKER_alpha_9f3a2b_payload";

class AlphaModule final : public Module
{
public:
    [[nodiscard]] std::string_view name() const override { return "alpha"; }

    // The observable side effect that keeps the marker alive under LTO when alpha IS referenced.
    void on_register(World& /*world*/, EventBus& /*events*/) override { std::puts(kAlphaMarker); }
};
} // namespace

// Explicit registration entry point. The generated registration TU names this symbol ONLY when a
// build references package "alpha"; otherwise alpha's object is never pulled from its archive.
void register_alpha(Kernel& kernel) { kernel.add_module(std::make_unique<AlphaModule>()); }

} // namespace context::kernel::registration
