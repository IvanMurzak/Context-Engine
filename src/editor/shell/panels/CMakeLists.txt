# src/editor/shell/panels/ — M9 e05d1: the panel COMPOSITION ROOT (context_editor_panels).
#
# The library that binds the Shell's panel-agnostic PanelHost to the concrete C++ panel libraries.
# See include/context/editor/shell/panels/builtin_panels.h for the full rationale; the CMake fact
# that matters here is the D10 one:
#
#   THIS is the only target in the Shell family that links a `context_gui_panel_*` library, and it is
#   linked by the EXECUTABLES (context_editor + the CEF smoke), never by context_editor_shell. Two of
#   today's panels (scenetree, inspector) link `context_compose`, which the shell-boundary gate
#   FORBIDS, so keeping panel libraries off context_editor_shell's closure is what lets e05d3 host
#   them by editing this directory rather than by weakening the gate.
#
# Every library on this target's closure today is boundary-clean:
#   context_editor_shell     -> client / render_present / gui_uitree / gui_contract
#   context_gui_panel_problems -> gui_uitree / bridge          (bridge is deliberately NOT forbidden)
# so `context_editor`'s audited closure stays CLEAN and the gate's FORBIDDEN list is untouched.

add_library(context_editor_panels STATIC
    src/problems_feed.cpp    # the live `diagnostics` topic -> ProblemsPanel projection (04 §4)
    src/builtin_panels.cpp)  # the provider bindings for every panel this build can render

target_include_directories(context_editor_panels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC on both: problems_feed.h names shell::PanelHost / shell::PanelProvider AND
# problems::ProblemsPanel in its public surface, so a consumer compiling against this header needs
# both include trees.
target_link_libraries(context_editor_panels
    PUBLIC context_editor_shell context_gui_panel_problems
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Registered in the `editor-shell-` family — a PLAIN (non-gate) family, so the build job's general
# ctest step auto-runs it on all three OS legs and `--preset dev` builds it: no ci.yml `--target`
# bookkeeping, so the "Not Run = RED" tripwire does not apply.
#
# Static GCC runtime for the same reason the sibling shell executables take it: context_bridge pulls
# std::filesystem, and a PATH-shadowed libstdc++-6.dll otherwise makes the local ctest gate
# non-deterministic (setup.md § Preconditions).
set(_context_editor_panels_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_editor_panels_tests
    test_problems_feed     # the wire->model projection, promotion/settle, and the row-id mapping
    test_builtin_panels)   # the composition root: what binds, what stays unhosted, end to end

foreach(_t IN LISTS _context_editor_panels_tests)
    add_executable(context_editor_panels_${_t} tests/${_t}.cpp)
    target_link_libraries(context_editor_panels_${_t}
        PRIVATE context_editor_panels context_warnings)
    target_link_options(context_editor_panels_${_t}
        PRIVATE ${_context_editor_panels_static_runtime})
    target_include_directories(context_editor_panels_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME editor-shell-${_t} COMMAND context_editor_panels_${_t})
endforeach()
