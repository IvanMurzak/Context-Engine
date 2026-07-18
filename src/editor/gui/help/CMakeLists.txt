# src/editor/gui/help/ — M8.5 a20: in-editor contextual help (context_gui_help, R-HUX-010 / R-CLI-013 /
# R-HUX-004 / R-QA-006, issue a20). A THIN help layer: the per-verb help a human reads in the editor is
# a pure projection of the ONE contract registry (src/editor/contract/), so it can never drift from
# `context describe` / `context <verb> --help`; the getting-started panel references the R-QA-006
# human-onboarding samples. NO new content pipeline, NO new third-party dependency, NO network (offline).
#
# Pure C++ over context_gui_uitree + context_contract — NO CEF — so it builds + unit-tests on the
# DEFAULT 3-OS matrix and the local dev gate, exactly like the sibling panels. Added BEFORE a11y in
# gui/CMakeLists.txt, which links + scans the registered Help panel.

add_library(context_gui_help STATIC
    src/help_model.cpp
    src/help_panel.cpp)

target_include_directories(context_gui_help PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC deps expose types in the public headers:
#   context_gui_uitree -> uitree::Panel                         (help_panel.h build_panel())
#   context_contract   -> contract::VerbSpec / contract::FlagSpec (help_model.h render_verb_help())
# context_warnings is PRIVATE (no warning-flag leak to a CEF-host consumer, per conventions.md). NO new
# third-party dependency (no vcpkg / license-allowlist change) — the registry is the only data source.
target_link_libraries(context_gui_help
    PUBLIC context_gui_uitree context_contract
    PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling gui/panels pattern);
# tests/help_test.h is the tiny CHECK() harness. help_model.cpp uses std::ostringstream and the
# getting-started test uses std::filesystem, so static-link the GCC runtime (gated to GNU) to keep the
# local ctest gate deterministic against a PATH-shadowed libstdc++-6.dll (setup.md § Preconditions).
set(_context_gui_help_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_help_tests
    test_help_model
    test_help_panel
    test_a11y)

foreach(_t IN LISTS _context_gui_help_tests)
    add_executable(context_gui_help_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_help_${_t}
        PRIVATE context_gui_help context_warnings)
    target_link_options(context_gui_help_${_t}
        PRIVATE ${_context_gui_help_static_runtime})
    target_include_directories(context_gui_help_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-help-${_t} COMMAND context_gui_help_${_t})
endforeach()

# The getting-started test additionally needs the repo root that holds samples/ (asserts every
# referenced sample directory exists — offline, rots-if-drift).
add_executable(context_gui_help_test_getting_started tests/test_getting_started.cpp)
target_link_libraries(context_gui_help_test_getting_started
    PRIVATE context_gui_help context_warnings)
target_link_options(context_gui_help_test_getting_started
    PRIVATE ${_context_gui_help_static_runtime})
target_include_directories(context_gui_help_test_getting_started
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(context_gui_help_test_getting_started PRIVATE
    CONTEXT_HELP_SAMPLES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
add_test(NAME gui-help-getting-started COMMAND context_gui_help_test_getting_started)

# The contextual cross-check links context_gui_a11y to assert help topics name the SAME panel set the
# a11y harness scans (registered_panels()) — the register-with-the-panel discipline applied to help.
# Forward-refs context_gui_a11y (added after help in gui/CMakeLists.txt); cross-directory linking is
# valid at the repo's CMake >= 3.25 floor.
add_executable(context_gui_help_test_contextual tests/test_contextual.cpp)
target_link_libraries(context_gui_help_test_contextual
    PRIVATE context_gui_help context_gui_a11y context_warnings)
target_link_options(context_gui_help_test_contextual
    PRIVATE ${_context_gui_help_static_runtime})
target_include_directories(context_gui_help_test_contextual
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME gui-help-contextual COMMAND context_gui_help_test_contextual)
