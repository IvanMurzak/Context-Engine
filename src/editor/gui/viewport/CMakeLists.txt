# src/editor/gui/viewport/ — M5-F1: the native viewport OBSERVER panel (context_gui_viewport,
# R-EDIT-001 / R-UI-007 / L-41 / R-REND-002 / R-HEAD-002 / R-A11Y-001 / R-HUX-011, issue #164).
# Projects the observer viewport into a headless context_gui_uitree Panel: the "live scene (3D+2D)"
# the viewport composites over context_render(_wgpu), the L-41 present outcome (via context_gui_
# compositor), a keyboard-reachable "frame scene" observer affordance driving the R-HUX-011
# gesture->viewport-update loop, and the reserved viewport.* codes. READ-ONLY OBSERVER — no world
# writes (in-viewport authoring is R-HUX-006 -> M8.5).
#
# Pure C++ over the F0b headless libs + the render RHI abstraction — NO CEF, NO GPU backend — so it
# builds + unit-tests on the DEFAULT 3-OS matrix and the local dev gate, exactly like the sibling
# panels. The render pixels + golden-scene equivalence are the render-layer proof (src/render/
# viewport_scene.h + goldens/viewport.ppm, gated by the CI render job); this panel never boots CEF.

add_library(context_gui_viewport STATIC
    src/viewport_model.cpp
    src/viewport_panel.cpp)

target_include_directories(context_gui_viewport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree     -> uitree::Panel                        (viewport_panel.h build_panel())
#   context_gui_compositor -> compositor::SurfaceHandoff/Platform  (viewport_model.h/panel.h present())
#   context_render         -> render::RenderSnapshot               (viewport_panel.h set_snapshot)
# context_warnings is PRIVATE (no flag leak to a CEF-host consumer, per conventions.md). NO new
# third-party dependency (no vcpkg / license-allowlist change). Does NOT link context_render_wgpu (the
# CI-only GPU backend) nor context_render_sprite — the panel only models the observer surface.
target_link_libraries(context_gui_viewport
    PUBLIC context_gui_uitree context_gui_compositor context_render
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/panels pattern);
# tests/viewport_test.h is the tiny CHECK() harness. context_render pulls std::filesystem/chrono
# transitively (kernel lock/daemon), so static-link the GCC runtime (gated to GNU) to keep the local
# ctest gate deterministic against a PATH-shadowed libstdc++-6.dll (setup.md § Preconditions).
set(_context_gui_viewport_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_viewport_tests
    test_viewport_model
    test_viewport_panel
    test_a11y)

foreach(_t IN LISTS _context_gui_viewport_tests)
    add_executable(context_gui_viewport_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_viewport_${_t}
        PRIVATE context_gui_viewport context_warnings)
    target_link_options(context_gui_viewport_${_t}
        PRIVATE ${_context_gui_viewport_static_runtime})
    target_include_directories(context_gui_viewport_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-viewport-${_t} COMMAND context_gui_viewport_${_t})
endforeach()

# =================================================================================================
# M8.5 a19 — in-context viewport OVERRIDE editing (context_gui_viewport_edit, R-HUX-006 MUST core;
# R-CLI-006 / L-35 / L-20 / L-30 / R-A11Y-001, issue authored by the a19 task). The GUI face of scene
# composition (L-35): a transform gizmo / property edit on a COMPOSED scene-instance entity commits an
# L-35 OVERRIDE through the SAME composed write path `context set` runs (compose::plan_write via the
# shared context_compose_project resolver), routed through inspector::commit_override_write (the ONE
# L-30 rebase-or-drop engine), with the R-CLI-006 provenance chain rendered at the point of edit.
# A SEPARATE library from the observer (context_gui_viewport) so the read-only observer stays light +
# free of the compose write-path / inspector dependency; both live in this directory.
add_library(context_gui_viewport_edit STATIC
    src/viewport_edit_model.cpp
    src/viewport_edit_panel.cpp
    src/project_override_gateway.cpp)

target_include_directories(context_gui_viewport_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree         -> uitree::Panel                      (viewport_edit_panel.h)
#   context_compose            -> compose::WriteTarget/ProvenanceEntry/SceneResolver (viewport_edit_model.h)
#   context_gui_panel_inspector-> inspector::OverrideWriteGateway/CommitResult + commit_override_write
#                                 (viewport_edit_model.h + project_override_gateway.h — the ONE L-30 engine)
#   context_serializer         -> serializer::JsonValue              (viewport_edit_model.h)
# PRIVATE: the disk-backed gateway's plan_write resolver + atomic write (impl-only), + warnings.
# context_gui_panel_builders (M9 e05d3): the disk-backed gateway converts the seam's boundary-clean
# OverrideWriteRequest through builders::to_write_request. PRIVATE — only the .cpps reach it.
target_link_libraries(context_gui_viewport_edit
    PUBLIC context_gui_uitree context_compose context_gui_panel_inspector context_serializer
    PRIVATE context_compose_project context_filesync context_gui_panel_builders context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# context_filesync (via the gateway) pulls std::filesystem transitively, so static-link the GCC
# runtime (gated to GNU) to keep the local ctest gate deterministic against a PATH-shadowed
# libstdc++-6.dll (setup.md § Preconditions), mirroring the observer tests above.
set(_context_gui_viewport_edit_tests
    test_viewport_edit_model
    test_viewport_edit_panel)

foreach(_t IN LISTS _context_gui_viewport_edit_tests)
    add_executable(context_gui_viewport_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_viewport_${_t}
        PRIVATE context_gui_viewport_edit context_warnings)
    target_link_options(context_gui_viewport_${_t}
        PRIVATE ${_context_gui_viewport_static_runtime})
    target_include_directories(context_gui_viewport_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-viewport-${_t} COMMAND context_gui_viewport_${_t})
endforeach()
