# spikes/cef-compositing — L-41 fifth M0 spike: CEF accelerated offscreen rendering (OSR)
# composited over an engine-rendered D3D11 viewport via shared textures (R-UI-007).
# THROWAWAY proof code.
#
# DOUBLY GATED and NEVER built in CI: CEF's pinned binary distribution is a ~162 MB download
# (~460 MB extracted; libcef.dll alone is 258 MB), so this spike hides behind its own
# CONTEXT_BUILD_SPIKE_CEF switch (OFF by default) in addition to CONTEXT_BUILD_SPIKES.
# The CI bench job (CONTEXT_BUILD_SPIKES=ON, CEF switch OFF) enters this directory and takes
# the early-return below — that configure-only exercise is the CI proof; the render/input
# proof runs locally on a Windows dev box (results: FINDINGS.md).
#
# Local build (Windows/MSVC only, Release config — the minimal CEF distro has no Debug libs):
#   cmake -S src -B src/build/cef -DCONTEXT_BUILD_SPIKE_CEF=ON
#   cmake --build src/build/cef --config Release --target cef-compositing-spike
#   src/build/cef/spikes/cef-compositing/Release/cef-compositing-spike.exe --autotest
# Optional: -DCONTEXT_SPIKE_CEF_ARCHIVE=<path-to-pre-downloaded .tar.bz2> to skip the download.
#
# CEF pin: 149.0.6+g0d0eeb6+chromium-149.0.7827.201, STABLE channel, windows64 *minimal*
# distribution from the Spotify CDN (the official CEF binary host). BSD-3-Clause — recorded in
# tools/license-allowlist.json (V8-spike precedent: acquired outside vcpkg, spike-only, never
# shipped; the L-42 from-source deviation is fine for throwaway spikes).

if(NOT CONTEXT_BUILD_SPIKE_CEF)
    message(STATUS "[cef-compositing] CONTEXT_BUILD_SPIKE_CEF=OFF — spike skipped "
                   "(configure-only exercise; the CEF binary distribution is never downloaded in CI).")
    return()
endif()

if(NOT (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    # The M0 spike measures the Windows D3D11 shared-handle path only (the MUST platform);
    # macOS (IOSurface) and Linux (dmabuf) are findings-grade research in FINDINGS.md.
    message(FATAL_ERROR "[cef-compositing] This spike builds on 64-bit Windows with MSVC only.")
endif()

include(FetchContent)

set(_cef_name "cef_binary_149.0.6+g0d0eeb6+chromium-149.0.7827.201_windows64_minimal")
# '+' must be %2B-escaped in the CDN URL.
set(_cef_url "https://cef-builds.spotifycdn.com/cef_binary_149.0.6%2Bg0d0eeb6%2Bchromium-149.0.7827.201_windows64_minimal.tar.bz2")
set(_cef_sha1 "fe8f461b743f03dc640e998ae08264407d8bc2c9") # published by the CDN index.json

set(CONTEXT_SPIKE_CEF_ARCHIVE "" CACHE FILEPATH
    "Pre-downloaded ${_cef_name}.tar.bz2 (skips the ~162 MB CDN download; same SHA1 enforced)")

if(CONTEXT_SPIKE_CEF_ARCHIVE)
    set(_cef_fetch_url "${CONTEXT_SPIKE_CEF_ARCHIVE}")
else()
    set(_cef_fetch_url "${_cef_url}")
endif()

# Knobs must be in the cache BEFORE FetchContent_MakeAvailable: the CEF distribution root has
# its own CMakeLists.txt, so MakeAvailable auto-add_subdirectory()s it, and THAT build defines
# the libcef_dll_wrapper target (do not add it again — CMP0002 duplicate-target error).
#   * /MD to match this repo's default MSVC runtime (cef_variables defaults to /MT, which
#     LNK2038-clashes with our CMake-default dynamic runtime);
#   * sandbox OFF: the spike runs CefSettings.no_sandbox=1 (the M138+ sandbox requires the
#     bootstrap.exe launch model — app-packaging concern, orthogonal to the compositing seam).
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "CEF MSVC runtime flag" FORCE)
option(USE_SANDBOX "" OFF)

FetchContent_Declare(cef
    URL "${_cef_fetch_url}"
    URL_HASH SHA1=${_cef_sha1}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(cef)

# CEF's CMake modules (FindCEF ran inside the distro's own CMakeLists during MakeAvailable;
# re-running it here surfaces CEF_* variables — CEF_LIB_RELEASE, CEF_BINARY_FILES, macros —
# in THIS scope).
set(CEF_ROOT "${cef_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

add_executable(cef-compositing-spike
    src/main.cpp
    src/renderer_d3d11.cpp)

target_compile_definitions(cef-compositing-spike PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# SYSTEM keeps the repo /W4 /WX baseline on OUR sources while exempting CEF headers.
target_include_directories(cef-compositing-spike SYSTEM PRIVATE "${CEF_ROOT}")
target_include_directories(cef-compositing-spike PRIVATE src)

target_link_libraries(cef-compositing-spike PRIVATE
    context_warnings
    libcef_lib
    libcef_dll_wrapper
    d3d11 dxgi d3dcompiler)

add_dependencies(cef-compositing-spike libcef_dll_wrapper)

# CEF runtime payload (libcef.dll + resources + locales) and the spike's test page must sit
# next to the exe.
set(_out_dir "$<TARGET_FILE_DIR:cef-compositing-spike>")
add_custom_command(TARGET cef-compositing-spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Release"   "${_out_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Resources" "${_out_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/ui.html" "${_out_dir}/ui.html"
    COMMENT "cef-compositing-spike: staging CEF binaries/resources + ui.html")
