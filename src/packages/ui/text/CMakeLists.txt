# src/packages/ui/text/ — context_ui_text: the M7 runtime-UI text substrate (a7, R-UI-005; issue #237).
# FreeType glyph rasterization + the run-based measure() seam + the embedded default fonts. A SIBLING
# STATIC lib of the context_ui foundation (the context_ui_script / context_ui_input precedent) so the
# pure-stdlib foundation never links FreeType. FreeType is linked PRIVATE and the public headers are
# FreeType-free (PIMPL), so nothing downstream (context_render_ui, the web target) inherits FreeType.
#
# Delivery channel (DECISION-a7a8-deps.md): FreeType is fetched + SHA-verified + built FROM SOURCE via
# its own first-class CMake (cmake/ContextFreetype.cmake) — portable C, so it builds under the local
# Strawberry-GCC dev gate AND all three CI build legs with no prebuilt. FTL license elected (never GPL);
# every optional subsystem disabled. The default fonts are the vendored upstream TTFs (third_party/
# fonts/) embedded as byte arrays by tools/embed_bin.py at build time — compiled-in, no runtime file IO.

include(${CMAKE_SOURCE_DIR}/../cmake/ContextFreetype.cmake) # creates the `freetype` target (once)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_ui_text_root "${CMAKE_SOURCE_DIR}/..")            # Context-Engine repo root (cmake -S src)
set(_ui_text_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_ui_text_gen}")
set(_ui_text_ns "context::packages::ui::text::detail")
set(_embed_py "${_ui_text_root}/tools/embed_bin.py")

# Generate the embedded-font TUs from the vendored TTFs. Deterministic (embed_bin.py fixes byte order +
# formatting), regenerated only when the .ttf or the generator changes.
set(_noto_sans_ttf "${_ui_text_root}/third_party/fonts/NotoSans-Regular.ttf")
set(_noto_arabic_ttf "${_ui_text_root}/third_party/fonts/NotoSansArabic-Regular.ttf")
set(_noto_sans_cpp "${_ui_text_gen}/noto_sans_regular.cpp")
set(_noto_arabic_cpp "${_ui_text_gen}/noto_sans_arabic_regular.cpp")

add_custom_command(
    OUTPUT "${_noto_sans_cpp}"
    COMMAND ${Python3_EXECUTABLE} "${_embed_py}" "${_noto_sans_ttf}" "${_noto_sans_cpp}"
            kNotoSansRegularTtf --namespace "${_ui_text_ns}"
    DEPENDS "${_noto_sans_ttf}" "${_embed_py}"
    COMMENT "Embedding NotoSans-Regular.ttf -> noto_sans_regular.cpp"
    VERBATIM)
add_custom_command(
    OUTPUT "${_noto_arabic_cpp}"
    COMMAND ${Python3_EXECUTABLE} "${_embed_py}" "${_noto_arabic_ttf}" "${_noto_arabic_cpp}"
            kNotoSansArabicRegularTtf --namespace "${_ui_text_ns}"
    DEPENDS "${_noto_arabic_ttf}" "${_embed_py}"
    COMMENT "Embedding NotoSansArabic-Regular.ttf -> noto_sans_arabic_regular.cpp"
    VERBATIM)

add_library(context_ui_text STATIC
    src/font.cpp
    src/measure.cpp
    src/embedded_fonts.cpp
    "${_noto_sans_cpp}"
    "${_noto_arabic_cpp}")
target_include_directories(context_ui_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# FreeType PRIVATE: the public headers are FreeType-free (PIMPL), so FreeType never propagates as a
# usage requirement. context_warnings PRIVATE — our code is warnings-as-errors; FreeType is not (it
# does not link context_warnings and its includes are marked SYSTEM in ContextFreetype.cmake).
target_link_libraries(context_ui_text PRIVATE freetype context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests) ----------------------------------------------
# The `ui-*` family auto-runs in the build job's general ctest step (not in the -E gate-exclusion
# regex, built by --preset dev), so no ci.yml edit is needed. Static-link the GCC runtime like the
# sibling packages (a stale libstdc++-6.dll on PATH can otherwise shadow-crash the exe at launch).
set(_ui_text_static_runtime $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
set(_ui_text_tests
    test_font       # face load from embedded bytes, glyph_index, metrics, advance, rasterize coverage
    test_measure)   # UTF-8 decode + the run-based measure() seam (glyph ids, advances, clusters, edges)
foreach(_t IN LISTS _ui_text_tests)
    add_executable(context_ui_${_t} tests/${_t}.cpp)
    target_link_libraries(context_ui_${_t} PRIVATE context_ui_text context_warnings)
    target_link_options(context_ui_${_t} PRIVATE ${_ui_text_static_runtime})
    target_include_directories(context_ui_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
    add_test(NAME ui-${_t} COMMAND context_ui_${_t})
endforeach()
