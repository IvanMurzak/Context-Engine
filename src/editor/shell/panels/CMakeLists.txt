# src/editor/shell/panels/ — M9 e05d1/e05d3: the panel COMPOSITION ROOT (context_editor_panels).
#
# The library that binds the Shell's panel-agnostic PanelHost to the concrete C++ panel libraries.
# See include/context/editor/shell/panels/builtin_panels.h for the full rationale; the CMake fact
# that matters here is the D10 one:
#
#   THIS is the only target in the Shell family that links `context_gui_panel_*` libraries, and it is
#   linked by the EXECUTABLES (context_editor + the CEF smoke), never by context_editor_shell. Since
#   e05d3 the three hosted panel libraries are ALL boundary-clean (the scenetree/inspector
#   kernel-typed builders were split out to gui/panels/builders/, which only the DAEMON side links),
#   so `context_editor`'s audited closure — which now includes them — stays CLEAN and the gate's
#   FORBIDDEN list is untouched. Do NOT link context_gui_panel_builders (or any kernel module) here:
#   that would put compose/schema straight back on the audited closure the split just cleaned.
#
# Every library on this target's closure is boundary-clean:
#   context_editor_shell        -> client / render_present / gui_uitree / gui_contract
#   context_gui_panel_problems  -> gui_uitree / bridge       (bridge is deliberately NOT forbidden)
#   context_gui_panel_scenetree -> gui_uitree / bridge       (e05d3: compose split OUT)
#   context_gui_panel_inspector -> gui_uitree / serializer   (e05d3: compose+schema split OUT)
#   context_gui_playbar         -> gui_uitree                (e08b: session_control split OUT, so the
#                                                             transport carries no runtime/render dep)

add_library(context_editor_panels STATIC
    src/problems_feed.cpp    # the live `diagnostics` topic -> ProblemsPanel projection (04 §4)
    src/scenetree_feed.cpp   # e05d3: the `editor.scene-tree` read -> SceneTreePanel projection
    src/inspector_feed.cpp   # e05d3: the `editor.inspect` read -> InspectorPanel projection
    src/session_feed.cpp     # e08b: the `session` topic + the `editor.*` writes <-> panel state
    src/builtin_panels.cpp)  # the provider bindings + the live-hydration pump

target_include_directories(context_editor_panels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC on all four: the feed headers name shell::PanelHost / shell::PanelProvider AND their
# panels' model types in their public surfaces, so a consumer compiling against them needs every
# include tree. (context_client arrives transitively PUBLIC via context_editor_shell — the pump's
# complete-type use is builtin_panels.cpp-local.)
target_link_libraries(context_editor_panels
    PUBLIC context_editor_shell context_gui_panel_problems context_gui_panel_scenetree
           context_gui_panel_inspector context_gui_playbar
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
    test_scenetree_feed    # e05d3: the scene-tree wire round-trip, settle->refetch, selection dispatch
    test_inspector_feed    # e05d3: the inspector wire round-trip, CAS token, edit staging dispatch
    test_session_feed      # e08b: the `session` facts, origin echo suppression, the `editor.*` writes
    test_builtin_panels)   # the composition root: what binds, what stays unhosted, end to end

# The two e05d3 feed tests ALSO link context_gui_panel_builders: the round-trip half builds models
# with the REAL kernel-side builders + wire serializers and asserts the feed parsers reconstruct
# them (tests are not on any audited closure — reaching compose there is exactly the split's point).
foreach(_t IN LISTS _context_editor_panels_tests)
    add_executable(context_editor_panels_${_t} tests/${_t}.cpp)
    target_link_libraries(context_editor_panels_${_t}
        PRIVATE context_editor_panels context_gui_panel_builders context_warnings)
    target_link_options(context_editor_panels_${_t}
        PRIVATE ${_context_editor_panels_static_runtime})
    # The client SDK's own tests/ dir supplies `mock_channel.h` — the SCRIPTED WIRE the e08b session
    # test drives a REAL client::Client over. Reusing it (rather than writing a second mock here) is
    # deliberate: that header carries the standing "a mock more capable than the daemon hides real
    # bugs" warning and the flat-attach-reply rule, and a private copy would drift off both.
    target_include_directories(context_editor_panels_${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../client/tests)
    add_test(NAME editor-shell-${_t} COMMAND context_editor_panels_${_t})
endforeach()
