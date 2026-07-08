# src/editor/pkg/ — the M3 R-SEC-005 engine-driven install path (issue #100). The security envelope
# for the TS scripting tier's package installs: parse package.json + the npm v3 package-lock.json,
# enforce pinned versions + lockfile integrity (incl. transitive), classify a scripts-requiring
# package native-tier (the L-49 consent gate), and extract each verified artifact WITHOUT running any
# lifecycle script (--ignore-scripts, structural — no code path invokes a shell/subprocess).
#
# Stdlib-only crypto (sha512.cpp / base64.cpp), so no third-party dependency trips the deny-by-
# default license gate (the same trade filesync's sha256 / intent-log HMAC made). Links
# context_filesync for sha256 (SRI's weaker algorithm) and context_contract for the JSON DOM the
# lockfile reader parses with; it emits the R-CLI-008 install.* codes by STRING (registered in
# editor/contract/error_catalog.cpp — the promote-a-local-string pattern), so it needs no back-edge
# from contract. Wired in src/CMakeLists.txt apart from the editor cluster so concurrent sibling
# tasks touching src/editor/ and src/ merge cleanly (consumers name the `context_editor_pkg` target,
# resolved at generate time regardless of add_subdirectory order).

add_library(context_editor_pkg STATIC
    src/base64.cpp
    src/integrity.cpp
    src/lockfile.cpp
    src/npm_install.cpp
    src/sha512.cpp
    src/tar.cpp)
target_include_directories(context_editor_pkg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# context_filesync (sha256) is used only inside integrity.cpp; context_contract (Json) only inside
# lockfile.cpp — neither type appears in a public pkg header, so both are PRIVATE. Every Context
# target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_editor_pkg
    PRIVATE context_filesync context_contract context_warnings)

# npm_install.cpp uses std::filesystem; on the Strawberry-GCC Windows host a stale PATH
# libstdc++-6.dll can shadow its symbols at ctest launch (STATUS_ENTRYPOINT_NOT_FOUND), so the
# ctest executables static-link the GCC runtime — gated to GNU so the MSVC/Clang CI legs are
# untouched (profile setup.md § the std::filesystem note).
set(_ctx_pkg_gcc_static "$<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>")

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern);
# tests/pkg_test.h is the tiny CHECK() harness.
# NOTE: no test target may contain "install"/"setup"/"update"/"patch" in its NAME — Windows UAC
# installer-detection auto-flags such an .exe as requiring elevation, so ctest cannot launch it under
# a non-elevated user (it fails "requires elevation"). The install-path suite is therefore named
# `test_npm_deps` (not `test_npm_install`). Same rule drives cli/test_pkg_command + ts/test_pkg_resolve.
set(_ctx_pkg_tests
    test_base64
    test_sha512
    test_integrity
    test_tar
    test_lockfile
    test_npm_deps)

foreach(_t IN LISTS _ctx_pkg_tests)
    add_executable(context_editor_pkg_${_t} tests/${_t}.cpp)
    target_link_libraries(context_editor_pkg_${_t} PRIVATE context_editor_pkg context_warnings)
    target_link_options(context_editor_pkg_${_t} PRIVATE ${_ctx_pkg_gcc_static})
    target_include_directories(context_editor_pkg_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME pkg-${_t} COMMAND context_editor_pkg_${_t})
endforeach()

# test_integrity builds a sha256 SRI fixture directly (to prove SRI verifies BOTH algorithms), so it
# reaches filesync's sha256 header (PRIVATE in context_editor_pkg, so not propagated through it).
target_link_libraries(context_editor_pkg_test_integrity PRIVATE context_filesync)
