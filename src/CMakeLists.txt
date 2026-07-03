# Context Game Engine — top-level build.
#
# Build files live in src/ (owner ruling 2026-07-02: the repo root stays minimal), so configure
# from the repo root with an explicit source dir:
#     cmake -S src --preset dev
# Sibling directories (spikes/, bench/, cmake/) are reached via ../ with explicit binary dirs.
#
# NOTE: vcpkg is deliberately NOT required to configure or build this skeleton. The manifest
# (src/vcpkg.json) activates only when the vcpkg toolchain file is passed explicitly
# (-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake); a plain configure works
# on a machine with no vcpkg at all.

cmake_minimum_required(VERSION 3.25)

project(ContextEngine
    VERSION 0.0.1
    DESCRIPTION "Context Game Engine — AI-first, file-authoritative, headless-complete"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CONTEXT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (baseline: ON)" ON)
option(CONTEXT_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (non-MSVC only)" OFF)
option(CONTEXT_TSAN "Build with ThreadSanitizer (non-MSVC only; mutually exclusive with CONTEXT_SANITIZE — TSan cannot combine with ASan in one binary)" OFF)
option(CONTEXT_BUILD_SPIKES "Build the M0 de-risking spike targets under spikes/ (opt-in; the default CI matrix stays spike-free; some spikes require the vcpkg toolchain with VCPKG_MANIFEST_FEATURES=spikes)" OFF)
option(CONTEXT_BUILD_SPIKE_WASM "Build ONLY spikes/wasm (subset of CONTEXT_BUILD_SPIKES; acquires its own runtimes — needs no vcpkg toolchain), so CI can exercise the WASM spike without configuring the vcpkg-dependent spikes" OFF)
option(CONTEXT_BUILD_SPIKE_CEF "Build the CEF accelerated-OSR compositing spike (L-41) — downloads the pinned ~162 MB CEF binary distribution; Windows/MSVC-only, local runs only, NEVER enabled in CI" OFF)

# spikes/wasm builds WAMR (a C library): the C language must be enabled HERE — at top-level
# scope, BEFORE any add_subdirectory — not inside the spike's directory. Enabling C mid-tree
# under a chainloaded vcpkg toolchain leaves earlier-processed directories with a stale
# no-C-compiler snapshot and generation fails with "No known features for C compiler".
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_WASM)
    enable_language(C)
endif()

# Warnings-as-errors baseline: the `context_warnings` interface target (../cmake/ContextWarnings.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextWarnings.cmake)

if(CONTEXT_SANITIZE AND CONTEXT_TSAN)
    message(FATAL_ERROR "CONTEXT_SANITIZE and CONTEXT_TSAN are mutually exclusive: ThreadSanitizer cannot be combined with ASan/UBSan in one binary (use the separate `sanitize` and `tsan` presets).")
endif()

# ASan exists on MSVC but UBSan does not, and MSVC has no ThreadSanitizer; both sanitizer
# presets are scoped to non-MSVC toolchains (design lock L-38: sanitizer/TSan CI runs on
# Linux/Clang).
if(MSVC AND (CONTEXT_SANITIZE OR CONTEXT_TSAN))
    message(FATAL_ERROR "CONTEXT_SANITIZE and CONTEXT_TSAN are only supported with non-MSVC toolchains (Clang/GCC).")
endif()

if(CONTEXT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(CONTEXT_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

enable_testing()

# spikes/ lives OUTSIDE the source dir (sibling of src/), so every spike is added with an
# explicit binary dir; the ${CMAKE_BINARY_DIR}/spikes/<name> layout keeps output paths
# identical to the pre-move tree (the CI bench job depends on <binaryDir>/spikes/parse-bench/...).
function(add_spike name)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../spikes/${name} ${CMAKE_BINARY_DIR}/spikes/${name})
endfunction()

# M0: the only always-on target is the hello spike.
add_spike(hello)

# M1: the microkernel core — the first real engine component (src/kernel/). R-KERNEL-001's ~6
# stable interfaces; src/editor/, src/runtime/, src/packages/ gain their targets in later M1 tasks.
add_subdirectory(kernel)

# M0 de-risking spikes are OPT-IN (CONTEXT_BUILD_SPIKES=ON): the default CI matrix stays
# untouched. spikes/ecs takes its dependencies from the vcpkg manifest's `spikes` feature;
# spikes/js-engine acquires its pinned backends itself (FetchContent + NuGet — see its README);
# spikes/webgpu downloads the pinned wgpu-native prebuilt itself (no vcpkg port exists) and is
# also standalone-configurable for its Emscripten web leg — see its CMakeLists.txt.
if(CONTEXT_BUILD_SPIKES)
    add_spike(ecs)
    add_spike(parse-bench)
    add_spike(js-engine)
    add_spike(webgpu)
endif()

# spikes/wasm acquires its own pinned runtimes (wasmtime prebuilt C API + WAMR from source) and
# needs no vcpkg toolchain, so it also has a stand-alone toggle the spikes CI job uses.
# (The C language WAMR needs is enabled at the top of this file, before any add_subdirectory.)
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_WASM)
    add_spike(wasm)
endif()

# spikes/cef-compositing (L-41) is DOUBLY gated: CEF's binary distribution is a ~162 MB
# download, so the spike hides behind its own CONTEXT_BUILD_SPIKE_CEF switch and is never
# built in CI. With CONTEXT_BUILD_SPIKES=ON but the CEF switch OFF (the CI bench job) the
# directory is entered and returns early — configure-only exercise, no download. With
# CONTEXT_BUILD_SPIKE_CEF=ON alone (the local Windows run) the spike builds WITHOUT dragging
# in the vcpkg-dependent spikes above.
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_CEF)
    add_spike(cef-compositing)
endif()

# ---------------------------------------------------------------------------
# R-VER-004 — versioned install layout (docs/versioned-install.md).
#
# Every engine version installs SIDE-BY-SIDE under <prefix>/versions/<semver>/ and NEVER
# flat — a first release that installed flat would foreclose side-by-side installs forever,
# so the LAYOUT is the day-one contract. The resolver/fetcher/launcher machinery lands with
# the second release; this is the minimal install/package skeleton that stakes out the
# convention now. R-SEC-009: when the on-demand version fetcher lands, a fetched version is
# verify-before-use / fail closed via tools/verify_artifact.py before it is ever executed.
# ---------------------------------------------------------------------------
# R-VER-004 side-by-side install root: <prefix>/versions/<semver>/. A PLAIN (non-cache)
# variable so it is recomputed from PROJECT_VERSION on EVERY configure. A
# `set(... CACHE STRING ...)` without FORCE is a no-op once the cache entry exists, so
# re-configuring an existing build dir after a version bump would strand this at the OLD
# version while context-version.txt / CPACK_PACKAGE_VERSION (rewritten below from
# PROJECT_VERSION every configure) track the new one — the versioned dir name and its own
# marker would then disagree, the exact invariant cmake/versioned-install-check.cmake guards.
# The layout is a fixed day-one contract, so it is intentionally NOT overridable via -D.
set(CONTEXT_INSTALL_SUBDIR "versions/${PROJECT_VERSION}")

# Machine-readable version marker: a resolver enumerates installed versions by listing
# <prefix>/versions/*/ — the day-one hook the future launcher resolves the R-VER-003 pin
# against.
set(_context_version_marker "${CMAKE_CURRENT_BINARY_DIR}/context-version.txt")
file(WRITE "${_context_version_marker}"
     "name=Context Engine\nversion=${PROJECT_VERSION}\nlayout=R-VER-004\n")

# M0 has one always-on target (context-hello); it stands in as the installed payload so the
# layout is proven by a real file landing under versions/<semver>/bin/. Engine components
# (kernel/editor/runtime/packages) join this install set as they gain targets from M1 — the
# microkernel library (context_kernel) is the first to do so.
install(TARGETS context-hello context_kernel
    RUNTIME DESTINATION "${CONTEXT_INSTALL_SUBDIR}/bin"
    LIBRARY DESTINATION "${CONTEXT_INSTALL_SUBDIR}/lib"
    ARCHIVE DESTINATION "${CONTEXT_INSTALL_SUBDIR}/lib")
install(FILES "${_context_version_marker}" DESTINATION "${CONTEXT_INSTALL_SUBDIR}")

# CPack packaging: the produced archive carries the SAME versions/<semver>/ internal layout
# (it consumes the install() rules above), so `cpack -G ZIP` and `cmake --install` agree by
# construction. Kept minimal — a built-in ZIP archive, no external packaging tools.
set(CPACK_PACKAGE_NAME "context-engine")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE.md")
set(CPACK_GENERATOR "ZIP")
include(CPack)

# CI proof (runs under `ctest --preset dev` on all three OS legs of the CI build matrix):
# stage a real install into a build-local prefix and assert it landed under
# versions/<semver>/ and NOT flat. Fails the build on any R-VER-004 layout violation.
add_test(NAME versioned-install-layout
    COMMAND "${CMAKE_COMMAND}"
        -D "CONTEXT_BINARY_DIR=${CMAKE_BINARY_DIR}"
        -D "CONTEXT_EXPECTED_VERSION=${PROJECT_VERSION}"
        -D "CONTEXT_INSTALL_SUBDIR=${CONTEXT_INSTALL_SUBDIR}"
        -D "CONTEXT_BUILD_CONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/versioned-install-check.cmake")
