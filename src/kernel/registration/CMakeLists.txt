# src/kernel/registration/ — R-KERNEL-003 generated-registration DCE proof.
#
# Proves the microkernel's build-time dead-code-elimination promise: an UNREFERENCED package leaves
# zero footprint in the shipped binary. The mechanism (all three parts required by R-KERNEL-003):
#   1. A build-time GENERATED registration TU (generate_registration.cmake) that registers EXACTLY
#      the packages a build references — static-initializer self-registration is prohibited.
#   2. Uniform LTO (where the toolchain supports it) + linker garbage collection applied to the
#      proof targets (context_dce_flags below).
#   3. A measured referenced-vs-unreferenced size/symbol delta (the kernel-dce-footprint ctest),
#      asserting the unreferenced package contributes zero symbols to the final link.
#
# Two probe executables are built that differ ONLY in the generated registration TU each links:
#   context_dce_probe_referenced   registers {alpha, beta}
#   context_dce_probe_unreferenced registers {beta}          (alpha is the unreferenced package)
# Both link the SAME package archives, so the single variable is "does the generated TU name alpha".

# --- LTO + linker-GC flags (uniform across the proof targets) ----------------------------------
# The zero-footprint RESULT does not depend on LTO — archive-member link semantics + linker GC
# already deliver it — but LTO is part of the R-KERNEL-003 shipped-build pipeline, so it is applied
# uniformly to the proof targets when the toolchain supports it.
add_library(context_dce_flags INTERFACE)
if(MSVC)
    # /Gy: function-level linking (COMDAT per function); /Gw: per-data COMDAT. /OPT:REF drops
    # unreferenced COMDATs; /OPT:ICF folds identical ones.
    target_compile_options(context_dce_flags INTERFACE /Gy /Gw)
    target_link_options(context_dce_flags INTERFACE /OPT:REF /OPT:ICF)
else()
    # Put every function/data in its own section so the linker can collect the unreferenced ones.
    target_compile_options(context_dce_flags INTERFACE -ffunction-sections -fdata-sections)
    if(APPLE)
        target_link_options(context_dce_flags INTERFACE -Wl,-dead_strip)
    else()
        target_link_options(context_dce_flags INTERFACE -Wl,--gc-sections)
    endif()
endif()

# Uniform LTO where supported (portable check — falls back cleanly on toolchains that cannot archive
# LTO objects, e.g. some MinGW `ar` configurations, so the proof never goes red on LTO alone).
include(CheckIPOSupported)
check_ipo_supported(RESULT _context_dce_ipo OUTPUT _context_dce_ipo_msg LANGUAGES CXX)
if(_context_dce_ipo)
    message(STATUS "DCE proof: interprocedural optimization (LTO) ENABLED for the proof targets")
else()
    message(STATUS "DCE proof: LTO not supported by this toolchain (${_context_dce_ipo_msg}); "
                   "relying on archive-member semantics + linker GC")
endif()

# --- demo packages: each is its own STATIC library --------------------------------------------
# Archive-member link semantics are the primary elimination mechanism: an object in a static archive
# is pulled into the link ONLY when it resolves an undefined symbol. The generated TU creates those
# references, so an unreferenced package's archive member is never pulled.
foreach(_pkg alpha beta)
    add_library(context_dce_pkg_${_pkg} STATIC packages/${_pkg}.cpp)
    target_link_libraries(context_dce_pkg_${_pkg}
        PUBLIC context_kernel
        PRIVATE context_warnings context_dce_flags)
    target_include_directories(context_dce_pkg_${_pkg} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(_context_dce_ipo)
        set_property(TARGET context_dce_pkg_${_pkg} PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endforeach()

# --- probe variants: generated registration TU + shared probe_main -----------------------------
set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${_gen_dir})

# variant = referenced|unreferenced ; packages = comma-list ; expected = registered module count
function(_context_dce_add_probe variant packages expected)
    set(_out ${_gen_dir}/generated_registration_${variant}.cpp)
    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND}
            -DPACKAGES=${packages}
            -DOUT=${_out}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/generate_registration.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generate_registration.cmake
        COMMENT "Generating referenced-only registration TU (${variant}: ${packages})"
        VERBATIM)
    add_executable(context_dce_probe_${variant} probe_main.cpp ${_out})
    target_link_libraries(context_dce_probe_${variant}
        PRIVATE context_kernel context_dce_pkg_alpha context_dce_pkg_beta
                context_warnings context_dce_flags)
    target_include_directories(context_dce_probe_${variant} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(context_dce_probe_${variant}
        PRIVATE CONTEXT_DCE_EXPECTED_MODULES=${expected})
    if(_context_dce_ipo)
        set_property(TARGET context_dce_probe_${variant} PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
    # Self-check ctest: static-init guard (registry empty before explicit registration) + expected
    # referenced-package count (happy path for the referenced variant; edge/zero-extra for unref).
    add_test(NAME kernel-dce-probe-${variant} COMMAND context_dce_probe_${variant})
endfunction()

_context_dce_add_probe(referenced   "alpha,beta" 2)
_context_dce_add_probe(unreferenced "beta"       1)

# --- the measured footprint proof (referenced-vs-unreferenced symbol/size delta) ----------------
add_test(NAME kernel-dce-footprint
    COMMAND ${CMAKE_COMMAND}
        -DREFERENCED_BIN=$<TARGET_FILE:context_dce_probe_referenced>
        -DUNREFERENCED_BIN=$<TARGET_FILE:context_dce_probe_unreferenced>
        -DALPHA_MARKER=CONTEXT_DCE_MARKER_alpha_9f3a2b_payload
        -DBETA_MARKER=CONTEXT_DCE_MARKER_beta_7c1d4e_payload
        -P ${CMAKE_CURRENT_SOURCE_DIR}/dce_footprint_check.cmake)
