# src/editor/gui/playbar/ — M5-F5: the play-in-editor PLAYBAR + runtime session-control (context_gui_
# playbar, R-EDIT-001 / R-PLAY-001/002/003/004 / L-51 / L-22 / R-A11Y-001 / R-HUX-011, issue #166).
# Projects the play transport (play/pause/stop/step) into a headless context_gui_uitree Panel, drives
# session-control over src/runtime/session/ through the SessionControl seam, honors the L-51 edit/play
# split + L-22 hot reload, and yields the rendered play frame the F1 viewport observes (the SAME
# render::RenderSnapshot — no second render path). Mints the reserved play.* codes. Observer-grade play
# only (the M5 T1 loop) — no timeline/debugger/profiler UI.
#
# Pure C++ over the F0b headless libs + the runtime session + the render extract — NO CEF, NO GPU
# backend — so it builds + unit-tests on the DEFAULT 3-OS matrix and the local dev gate, exactly like
# the sibling viewport / inspector panels. The session-control + hot-reload logic is covered headless by
# context_gui_uitree + runtime-session fault-injection (no GPU required).

add_library(context_gui_playbar STATIC
    src/playbar_model.cpp
    src/playbar_panel.cpp
    src/session_control.cpp)

target_include_directories(context_gui_playbar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree -> uitree::Panel                        (playbar_panel.h build_playbar_panel())
#   context_render     -> render::RenderSnapshot               (session_control.h PlayFrame)
#   context_session    -> runtime::session::Session/Config     (session_control.h RuntimeSessionControl)
# context_warnings is PRIVATE (no flag leak to a CEF-host consumer, per conventions.md). NO new
# third-party dependency (no vcpkg / license-allowlist change). Does NOT link context_render_wgpu (the
# CI-only GPU backend) — the playbar only drives the session + extracts the observer snapshot.
target_link_libraries(context_gui_playbar
    PUBLIC context_gui_uitree context_render context_session
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/viewport pattern);
# tests/playbar_test.h is the tiny CHECK() harness. context_session + context_render pull
# std::filesystem/chrono transitively (kernel lock/daemon, replay/filesync jailing), so static-link the
# GCC runtime (gated to GNU) to keep the local ctest gate deterministic against a PATH-shadowed
# libstdc++-6.dll (setup.md § Preconditions). Target/exe names avoid the install/setup/update/patch
# tokens that trigger Windows UAC auto-elevation under ctest (setup.md).
set(_context_gui_playbar_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_playbar_tests
    test_playbar_model
    test_session_control
    test_a11y)

foreach(_t IN LISTS _context_gui_playbar_tests)
    add_executable(context_gui_playbar_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_playbar_${_t}
        PRIVATE context_gui_playbar context_warnings)
    target_link_options(context_gui_playbar_${_t}
        PRIVATE ${_context_gui_playbar_static_runtime})
    target_include_directories(context_gui_playbar_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-playbar-${_t} COMMAND context_gui_playbar_${_t})
endforeach()

# The panel test additionally links context_gui_viewport to assert the play frame flows through the F1
# viewport with no second render path (ViewportPanel::set_snapshot over the SAME render::RenderSnapshot).
add_executable(context_gui_playbar_test_playbar_panel tests/test_playbar_panel.cpp)
target_link_libraries(context_gui_playbar_test_playbar_panel
    PRIVATE context_gui_playbar context_gui_viewport context_warnings)
target_link_options(context_gui_playbar_test_playbar_panel
    PRIVATE ${_context_gui_playbar_static_runtime})
target_include_directories(context_gui_playbar_test_playbar_panel
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME gui-playbar-test_playbar_panel COMMAND context_gui_playbar_test_playbar_panel)
