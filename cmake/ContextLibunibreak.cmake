# cmake/ContextLibunibreak.cmake — build VENDORED libunibreak (UAX #14 line breaking).
#
# The M7 runtime-UI text shaper (src/packages/ui/text/, issue #237; a8) needs line-break opportunities
# (UAX #14) to wrap shaped runs. HarfBuzz does NOT do line breaking itself. DECISION-a7a8-deps.md picks
# libunibreak (Zlib) and cites the miniaudio precedent: it is a small C lib VENDORED into the repo
# (third_party/libunibreak/), not a fetched prebuilt — so no network/SHA-churn risk and it builds under
# every toolchain incl. the local Strawberry-GCC dev gate and the Emscripten web target.
#
# Only the LINE-BREAK closure is compiled (not word/grapheme breaking): linebreak + its data + the
# unibreak base/def + the East-Asian-width + emoji tables it consults. Dictionary-based breaking
# (Thai/Lao/Khmer) is declared OUT of scope (a8 spec). Public API is <linebreak.h>.
#
# LICENSE: Zlib (already allowlisted), vendored verbatim as third_party/libunibreak/LICENCE.
#
# Creates the `libunibreak` target, linked PRIVATE by context_ui_text.

if(TARGET libunibreak)
    return()
endif()

# The top-level project is CXX-only; libunibreak is C. Enable C here (idempotent) so the .c target compiles.
enable_language(C)

set(_lub_root "${CMAKE_CURRENT_LIST_DIR}/../third_party/libunibreak")
if(NOT EXISTS "${_lub_root}/src/linebreak.c")
    message(FATAL_ERROR "ContextLibunibreak: vendored source missing at ${_lub_root}/src/linebreak.c")
endif()

add_library(libunibreak STATIC
    "${_lub_root}/src/linebreak.c"
    "${_lub_root}/src/linebreakdata.c"
    "${_lub_root}/src/linebreakdef.c"
    "${_lub_root}/src/unibreakbase.c"
    "${_lub_root}/src/unibreakdef.c"
    "${_lub_root}/src/eastasianwidthdef.c"
    "${_lub_root}/src/eastasianwidthdata.c"
    "${_lub_root}/src/emojidef.c"
    "${_lub_root}/src/emojidata.c")
target_include_directories(libunibreak SYSTEM PUBLIC "${_lub_root}/src")
# Third-party C: does NOT link context_warnings; suppress its own warning noise under our default flags.
if(MSVC)
    target_compile_options(libunibreak PRIVATE /w)
else()
    target_compile_options(libunibreak PRIVATE -w)
endif()
set_target_properties(libunibreak PROPERTIES EXCLUDE_FROM_ALL ON)
