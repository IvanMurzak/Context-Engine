// The package-registration seam (R-KERNEL-003): the build-time GENERATED translation unit defines
// register_referenced_packages() to register EXACTLY the packages a build references — nothing else
// is linked in. Static-initializer self-registration is deliberately absent (it defeats --gc-sections).

#pragma once

#include "context/kernel/kernel.h"

namespace context::kernel::registration
{

// Implemented by the build-time generated TU (see src/kernel/registration/CMakeLists.txt +
// generate_registration.cmake). Registers each referenced package's Module onto `kernel` via an
// explicit register_<pkg>(Kernel&) call. An UNREFERENCED package's register_<pkg> is never named
// there, so archive-member link semantics + linker garbage collection drop its code entirely — the
// unreferenced package leaves zero footprint in the final binary.
void register_referenced_packages(Kernel& kernel);

} // namespace context::kernel::registration
