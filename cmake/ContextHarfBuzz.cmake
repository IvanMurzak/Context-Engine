# cmake/ContextHarfBuzz.cmake — fetch + SHA-verify + build HarfBuzz's AMALGAMATED single TU.
#
# The M7 runtime-UI text shaper (src/packages/ui/text/, issue #237; a8 text-shaping) shapes runs with
# HarfBuzz. DECISION-a7a8-deps.md mandates the amalgamated single-TU build: we compile ONLY
# src/harfbuzz.cc (which #includes the rest of HarfBuzz's src/ tree) into one static `harfbuzz` target,
# NOT HarfBuzz's own heavyweight CMake. Like FreeType it is portable C++ built FROM SOURCE (the
# WAMR/FreeType-from-pinned-source precedent), so it compiles under the local Strawberry-GCC dev gate,
# all three CI build legs, AND the Emscripten web target with no prebuilt and no toggle.
#
# NO external Unicode/ICU/glib/FreeType provider is enabled (no HAVE_* defines): the amalgamated TU uses
# HarfBuzz's built-in OpenType shaper + its bundled Unicode Character Database (hb-ucd.cc), so no
# transitive dependency enters the shipped engine (R-SEC-006; the FreeType<->HarfBuzz circular option
# stays severed — FreeType is built FT_DISABLE_HARFBUZZ). The pin lives in tools/harfbuzz-source.json
# (single source of truth); this module parses it with string(JSON) so the version/SHA are never duplicated.
#
# LICENSE: HarfBuzz is "Old MIT" (SPDX MIT-Modern-Variant), vendored verbatim as third_party/harfbuzz/COPYING.
#
# Creates the `harfbuzz` target (INTERFACE include dir = HarfBuzz src/, exported SYSTEM), linked PRIVATE
# by context_ui_text.

if(TARGET harfbuzz)
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/ContextDownload.cmake")

# --- read the pin (single source of truth) --------------------------------------------------------
set(_hb_pin "${CMAKE_CURRENT_LIST_DIR}/../tools/harfbuzz-source.json")
if(NOT EXISTS "${_hb_pin}")
    message(FATAL_ERROR "ContextHarfBuzz: pin file not found at ${_hb_pin}")
endif()
file(READ "${_hb_pin}" _hb_pin_json)
string(JSON _hb_version GET "${_hb_pin_json}" version)
string(JSON _hb_url     GET "${_hb_pin_json}" url)
string(JSON _hb_sha     GET "${_hb_pin_json}" sha256)

set(_hb_deps "${CMAKE_BINARY_DIR}/_deps")
set(_hb_tarball "${_hb_deps}/harfbuzz-${_hb_version}.tar.xz")
set(_hb_src "${_hb_deps}/harfbuzz-${_hb_version}")

# --- fetch + verify + extract (idempotent — skip when already extracted) ---------------------------
if(NOT EXISTS "${_hb_src}/src/harfbuzz.cc")
    file(MAKE_DIRECTORY "${_hb_deps}")
    context_download(
        URL             "${_hb_url}"
        PATH            "${_hb_tarball}"
        EXPECTED_SHA256 "${_hb_sha}"
        DESCRIPTION     "HarfBuzz ${_hb_version} source")
    message(STATUS "ContextHarfBuzz: extracting HarfBuzz ${_hb_version}")
    file(ARCHIVE_EXTRACT INPUT "${_hb_tarball}" DESTINATION "${_hb_deps}")
endif()
if(NOT EXISTS "${_hb_src}/src/harfbuzz.cc")
    message(FATAL_ERROR "ContextHarfBuzz: extracted tree missing src/harfbuzz.cc at ${_hb_src}")
endif()

# --- build the amalgamated single TU --------------------------------------------------------------
# EXCLUDE_FROM_ALL: harfbuzz builds only because context_ui_text links it. It is THIRD-PARTY code, so it
# does NOT link context_warnings; suppress its own warning noise entirely (-w / /w) since we compile it
# under our default flags (it is not warnings-as-errors, but keep the build output clean). HB_NO_MT keeps
# the shaper single-threaded (no mutex/thread-local machinery) — our use is synchronous and single-thread.
add_library(harfbuzz STATIC "${_hb_src}/src/harfbuzz.cc")
target_compile_definitions(harfbuzz PRIVATE HB_NO_MT)
target_include_directories(harfbuzz SYSTEM PUBLIC "${_hb_src}/src")
if(MSVC)
    target_compile_options(harfbuzz PRIVATE /w)
else()
    target_compile_options(harfbuzz PRIVATE -w)
endif()
set_target_properties(harfbuzz PROPERTIES EXCLUDE_FROM_ALL ON)
