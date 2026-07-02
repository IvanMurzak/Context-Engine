# Context Game Engine — top-level build.
#
# NOTE: vcpkg is deliberately NOT required to configure or build this skeleton. The manifest
# (vcpkg.json) activates only when the vcpkg toolchain file is passed explicitly
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
option(CONTEXT_BUILD_SPIKES "Build M0 measurement spikes (needs the vcpkg toolchain; OFF keeps the default CI matrix dependency-free)" OFF)

# Warnings-as-errors baseline: the `context_warnings` interface target (cmake/ContextWarnings.cmake).
include(cmake/ContextWarnings.cmake)

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

# M0: the only unconditional target is the hello spike. Engine components (kernel/,
# editor/, runtime/, packages/) gain targets starting at M1.
add_subdirectory(spikes/hello)

# Measurement spikes are opt-in so the default matrix stays vcpkg-free.
if(CONTEXT_BUILD_SPIKES)
    add_subdirectory(spikes/parse-bench)
endif()
