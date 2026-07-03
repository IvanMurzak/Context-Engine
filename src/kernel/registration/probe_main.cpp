// DCE proof probe: registers exactly the packages the (variant-specific) generated TU references,
// and self-checks the R-KERNEL-003 invariants. Shared by BOTH probe variants — the only difference
// between them is the generated registration TU each links (and the expected module count passed as
// CONTEXT_DCE_EXPECTED_MODULES).

#include "context/kernel/registration/registrar.h"

#include <cstddef>
#include <cstdio>

#ifndef CONTEXT_DCE_EXPECTED_MODULES
#define CONTEXT_DCE_EXPECTED_MODULES 0
#endif

int main()
{
    context::kernel::Kernel kernel;

    // GUARD (R-KERNEL-003): no package may self-register at static-init time — the registry MUST be
    // empty until the generated TU explicitly registers the referenced packages. A non-empty
    // registry here would mean a prohibited static-initializer self-registration slipped in.
    if (kernel.modules().size() != 0)
    {
        std::fprintf(stderr,
                     "FAIL: module registry non-empty before explicit registration "
                     "(static-initializer self-registration is prohibited)\n");
        return 2;
    }

    context::kernel::registration::register_referenced_packages(kernel);

    const std::size_t expected = CONTEXT_DCE_EXPECTED_MODULES;
    if (kernel.modules().size() != expected)
    {
        std::fprintf(stderr, "FAIL: expected %zu referenced package(s), registry has %zu\n",
                     expected, kernel.modules().size());
        return 1;
    }

    std::printf("OK: %zu referenced package(s) registered\n", kernel.modules().size());
    return 0;
}
