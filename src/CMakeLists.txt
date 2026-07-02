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
option(CONTEXT_BUILD_SPIKES "Build the M0 de-risking spike targets under spikes/ (opt-in; the default CI matrix stays spike-free; some spikes require the vcpkg toolchain with VCPKG_MANIFEST_FEATURES=spikes)" OFF)
option(CONTEXT_BUILD_SPIKE_CEF "Build the CEF accelerated-OSR compositing spike (L-41) — downloads the pinned ~162 MB CEF binary distribution; Windows/MSVC-only, local runs only, NEVER enabled in CI" OFF)

# Warnings-as-errors baseline: the `context_warnings` interface target (../cmake/ContextWarnings.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextWarnings.cmake)

if(CONTEXT_SANITIZE)
    if(MSVC)
        # ASan exists on MSVC but UBSan does not; the sanitize preset is scoped to non-MSVC
        # toolchains (design lock L-38 sanitizer CI runs on Linux/Clang).
        message(FATAL_ERROR "CONTEXT_SANITIZE is only supported with non-MSVC toolchains (Clang/GCC).")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

# M0: the only always-on target is the hello spike. Engine components (src/kernel/, src/editor/,
# src/runtime/, src/packages/) gain targets starting at M1.
# spikes/ lives OUTSIDE the source dir (sibling of src/), so every add_subdirectory names an
# explicit binary dir; the ${CMAKE_BINARY_DIR}/spikes/<x> layout keeps output paths identical
# to the pre-move tree (the CI bench job depends on build/bench/spikes/parse-bench/...).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../spikes/hello ${CMAKE_BINARY_DIR}/spikes/hello)

# M0 de-risking spikes are OPT-IN (CONTEXT_BUILD_SPIKES=ON): the default CI matrix stays
# untouched. spikes/ecs takes its dependencies from the vcpkg manifest's `spikes` feature;
# spikes/js-engine acquires its pinned backends itself (FetchContent + NuGet — see its README).
if(CONTEXT_BUILD_SPIKES)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../spikes/ecs ${CMAKE_BINARY_DIR}/spikes/ecs)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../spikes/parse-bench ${CMAKE_BINARY_DIR}/spikes/parse-bench)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../spikes/js-engine ${CMAKE_BINARY_DIR}/spikes/js-engine)
endif()

# spikes/cef-compositing (L-41) is DOUBLY gated: CEF's binary distribution is a ~162 MB
# download, so the spike hides behind its own CONTEXT_BUILD_SPIKE_CEF switch and is never
# built in CI. With CONTEXT_BUILD_SPIKES=ON but the CEF switch OFF (the CI bench job) the
# directory is entered and returns early — configure-only exercise, no download. With
# CONTEXT_BUILD_SPIKE_CEF=ON alone (the local Windows run) the spike builds WITHOUT dragging
# in the vcpkg-dependent spikes above.
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_CEF)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../spikes/cef-compositing ${CMAKE_BINARY_DIR}/spikes/cef-compositing)
endif()
