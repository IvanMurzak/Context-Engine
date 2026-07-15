# cmake/ContextFreetype.cmake — fetch + SHA-verify + build FreeType from its own first-class CMake.
#
# The M7 runtime-UI text substrate (src/packages/ui/text/, issue #237) rasterizes embedded TrueType
# fonts with FreeType. Unlike the MSVC/Clang-ABI native prebuilts (V8 / wgpu-native / CEF / wasmtime,
# fetched as opaque binaries), FreeType is portable C built FROM SOURCE (the WAMR-from-pinned-source
# precedent), so it compiles under the local Strawberry-GCC dev gate AND every CI build leg with no
# prebuilt and no toggle — the text package is part of the default build.
#
# LICENSE: FreeType is dual 'FTL OR GPL-2.0-or-later'; we ELECT FTL, never GPL (DECISION-a7a8-deps.md).
# We DISABLE every optional subsystem (embedded raw TTFs need none; severs the FreeType<->HarfBuzz
# circular option + minimizes surface, R-SEC-006). The pin lives in tools/freetype-source.json (single
# source of truth); this module parses it with string(JSON) so the version/SHA are never duplicated.
#
# Creates the `freetype` target (INTERFACE include dirs exported), linked PRIVATE by context_ui_text.

if(TARGET freetype)
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/ContextDownload.cmake")

# --- read the pin (single source of truth) --------------------------------------------------------
set(_ft_pin "${CMAKE_CURRENT_LIST_DIR}/../tools/freetype-source.json")
if(NOT EXISTS "${_ft_pin}")
    message(FATAL_ERROR "ContextFreetype: pin file not found at ${_ft_pin}")
endif()
file(READ "${_ft_pin}" _ft_pin_json)
string(JSON _ft_version GET "${_ft_pin_json}" version)
string(JSON _ft_url     GET "${_ft_pin_json}" url)
string(JSON _ft_sha     GET "${_ft_pin_json}" sha256)

set(_ft_deps "${CMAKE_BINARY_DIR}/_deps")
set(_ft_tarball "${_ft_deps}/freetype-${_ft_version}.tar.gz")
set(_ft_src "${_ft_deps}/freetype-${_ft_version}")

# --- fetch + verify + extract (idempotent — skip when already extracted) ---------------------------
if(NOT EXISTS "${_ft_src}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${_ft_deps}")
    context_download(
        URL             "${_ft_url}"
        PATH            "${_ft_tarball}"
        EXPECTED_SHA256 "${_ft_sha}"
        DESCRIPTION     "FreeType ${_ft_version} source")
    message(STATUS "ContextFreetype: extracting FreeType ${_ft_version}")
    file(ARCHIVE_EXTRACT INPUT "${_ft_tarball}" DESTINATION "${_ft_deps}")
endif()
if(NOT EXISTS "${_ft_src}/CMakeLists.txt")
    message(FATAL_ERROR "ContextFreetype: extracted tree missing CMakeLists.txt at ${_ft_src}")
endif()

# --- elect FTL + disable every optional subsystem before add_subdirectory --------------------------
# These map to FreeType's own options (it internally uses CMAKE_DISABLE_FIND_PACKAGE_* for each). FORCE
# so a stale cache from a prior configure cannot re-enable a subsystem.
set(FT_DISABLE_ZLIB     ON  CACHE BOOL "Context: no zlib in FreeType"     FORCE)
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "Context: no bzip2 in FreeType"    FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "Context: no libpng in FreeType"   FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "Context: no HarfBuzz in FreeType" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "Context: no brotli in FreeType"   FORCE)
set(FT_ENABLE_ERROR_STRINGS ON CACHE BOOL "Context: legible FT error strings" FORCE)
set(SKIP_INSTALL_ALL    ON  CACHE BOOL "Context: no install rules for the vendored build" FORCE)

# EXCLUDE_FROM_ALL: freetype builds only because context_ui_text links it (not part of `all` by itself).
add_subdirectory("${_ft_src}" "${_ft_deps}/freetype-build" EXCLUDE_FROM_ALL)

# Mark FreeType's exported include dirs as SYSTEM for consumers, so its C headers never trip our
# per-target -Wall -Wextra -Wpedantic -Werror / MSVC /W4 /WX baseline (context_warnings) when included
# from a Context TU. FreeType itself does NOT link context_warnings, so it builds under its own flags.
if(TARGET freetype)
    get_target_property(_ft_inc freetype INTERFACE_INCLUDE_DIRECTORIES)
    if(_ft_inc)
        set_target_properties(freetype PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_ft_inc}")
    endif()
endif()
