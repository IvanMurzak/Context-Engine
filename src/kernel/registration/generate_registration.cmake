# Build-time generator for the referenced-only registration TU (R-KERNEL-003). Invoked in CMake
# script mode by an add_custom_command:
#     cmake -DPACKAGES=<comma-list> -DOUT=<file> -P generate_registration.cmake
#
# Emits a translation unit defining context::kernel::registration::register_referenced_packages(),
# which calls register_<pkg>(kernel) for EXACTLY the packages named in PACKAGES — and forward-declares
# only those. A package absent from PACKAGES is never named, so its object is never pulled into the
# link (archive-member semantics) and the linker garbage-collects any residue. This IS the mechanism
# R-KERNEL-003 mandates: the build enumerates referenced packages and generates the registration TU.

if(NOT DEFINED OUT)
    message(FATAL_ERROR "generate_registration.cmake: OUT is required")
endif()

# PACKAGES is passed comma-separated (not semicolon) so it survives add_custom_command argument
# splitting; normalize it to a CMake list here.
set(_pkgs "")
if(DEFINED PACKAGES AND NOT PACKAGES STREQUAL "")
    string(REPLACE "," ";" _pkgs "${PACKAGES}")
endif()

set(_fwd "")
set(_calls "")
foreach(_pkg IN LISTS _pkgs)
    string(APPEND _fwd "void register_${_pkg}(Kernel&);\n")
    string(APPEND _calls "    register_${_pkg}(kernel);\n")
endforeach()

file(WRITE "${OUT}"
"// GENERATED FILE — DO NOT EDIT. Produced at build time by generate_registration.cmake
// (R-KERNEL-003 generated-registration DCE proof). Registers EXACTLY the referenced packages.

#include \"context/kernel/registration/registrar.h\"

namespace context::kernel::registration
{

${_fwd}
void register_referenced_packages(Kernel& kernel)
{
${_calls}    (void)kernel;
}

} // namespace context::kernel::registration
")
