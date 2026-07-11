# src/editor/gui/compositor/ — M5-F0b: the minimal L-41 surface-handoff seam (context_gui_compositor,
# R-UI-007 / L-41, issue #152). The headless model of the ratified per-platform CEF compositing tree
# (accelerated-OSR Win / IOSurface mac / software-OSR Linux behind the Mesa/X11-ozone gate). NOT full
# 3D compositing — the surface seam the F1 viewport later fills. Pure stdlib (no CEF), so it builds +
# tests on the DEFAULT 3-OS matrix; the selection logic is exercised for all three platforms on every
# OS via an explicit platform argument.

add_library(context_gui_compositor STATIC
    src/surface.cpp)

target_include_directories(context_gui_compositor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Warnings-as-errors baseline, PRIVATE (no flag leak to the CEF host consumer — conventions.md).
target_link_libraries(context_gui_compositor PRIVATE context_warnings)

# --- tests (R-QA-013) ---------------------------------------------------------------------------
add_executable(context_gui_compositor_test_surface tests/test_surface.cpp)
target_link_libraries(context_gui_compositor_test_surface
    PRIVATE context_gui_compositor context_warnings)
target_include_directories(context_gui_compositor_test_surface
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME gui-compositor-test_surface COMMAND context_gui_compositor_test_surface)
