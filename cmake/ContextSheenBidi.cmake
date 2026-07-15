# cmake/ContextSheenBidi.cmake — build VENDORED SheenBidi (UAX #9 bidi + UAX #24 script itemization).
#
# The M7 runtime-UI text shaper (src/packages/ui/text/, issue #237; a8) needs bidi run segmentation +
# reordering (UAX #9) and script itemization (UAX #24, SBScriptLocator). HarfBuzz does NEITHER itself.
# DECISION-a7a8-deps.md picks SheenBidi (Apache-2.0) over FriBidi (LGPL, excluded) and cites the
# miniaudio precedent: SheenBidi is a small C lib VENDORED into the repo (third_party/sheenbidi/), not a
# fetched prebuilt — so no network/SHA-churn risk (the GitHub source-archive is auto-generated) and it
# builds under every toolchain incl. the local Strawberry-GCC dev gate and the Emscripten web target.
#
# Built as the UNITY translation unit (SB_CONFIG_UNITY): only Source/SheenBidi.c is compiled; it #includes
# the rest of Source/*.c. Public API is <SheenBidi/SheenBidi.h> (Headers/ on the PUBLIC include path);
# the unity TU also needs Source/ on the path for its relative sibling includes.
#
# LICENSE: Apache-2.0 (already allowlisted), vendored verbatim as third_party/sheenbidi/LICENSE.
#
# Creates the `sheenbidi` target, linked PRIVATE by context_ui_text.

if(TARGET sheenbidi)
    return()
endif()

# The top-level project is CXX-only; SheenBidi is C. Enable C here (idempotent) so the .c target compiles.
enable_language(C)

set(_sb_root "${CMAKE_CURRENT_LIST_DIR}/../third_party/sheenbidi")
if(NOT EXISTS "${_sb_root}/Source/SheenBidi.c")
    message(FATAL_ERROR "ContextSheenBidi: vendored source missing at ${_sb_root}/Source/SheenBidi.c")
endif()

add_library(sheenbidi STATIC "${_sb_root}/Source/SheenBidi.c")
target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
# PUBLIC Headers/ so consumers resolve <SheenBidi/SheenBidi.h>; PRIVATE Source/ for the unity TU's
# relative sibling includes. Marked SYSTEM so the vendored C headers never trip our warnings baseline.
target_include_directories(sheenbidi SYSTEM PUBLIC "${_sb_root}/Headers")
target_include_directories(sheenbidi PRIVATE "${_sb_root}/Source")
# Third-party C: does NOT link context_warnings; suppress its own warning noise under our default flags.
if(MSVC)
    target_compile_options(sheenbidi PRIVATE /w)
else()
    target_compile_options(sheenbidi PRIVATE -w)
endif()
set_target_properties(sheenbidi PROPERTIES EXCLUDE_FROM_ALL ON)
