# R-VER-004 CI proof — invoked as a ctest test from src/CMakeLists.txt (add_test
# "versioned-install-layout"). Stages a real `cmake --install` into a build-local prefix
# and asserts the versioned side-by-side layout, failing closed (FATAL_ERROR ⇒ non-zero
# ctest) on any violation.
#
# Passed in via -D: CONTEXT_BINARY_DIR, CONTEXT_EXPECTED_VERSION, CONTEXT_INSTALL_SUBDIR,
# CONTEXT_BUILD_CONFIG.

set(_prefix "${CONTEXT_BINARY_DIR}/install-test")
file(REMOVE_RECURSE "${_prefix}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${CONTEXT_BINARY_DIR}"
            --prefix "${_prefix}" --config "${CONTEXT_BUILD_CONFIG}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (rc=${_rc}):\n${_out}\n${_err}")
endif()

set(_versioned "${_prefix}/${CONTEXT_INSTALL_SUBDIR}")

# 1. The versioned subtree must exist.
if(NOT IS_DIRECTORY "${_versioned}")
    message(FATAL_ERROR
        "R-VER-004 violation: expected versioned install dir '${_versioned}' does not exist")
endif()

# 2. The version marker must land INSIDE versions/<semver>/ and name the right version.
set(_marker "${_versioned}/context-version.txt")
if(NOT EXISTS "${_marker}")
    message(FATAL_ERROR "R-VER-004 violation: version marker missing at '${_marker}'")
endif()
file(READ "${_marker}" _marker_text)
if(NOT _marker_text MATCHES "version=${CONTEXT_EXPECTED_VERSION}")
    message(FATAL_ERROR
        "R-VER-004 violation: version marker does not record version=${CONTEXT_EXPECTED_VERSION}:\n${_marker_text}")
endif()

# 3. FAIL CLOSED on a FLAT install: nothing may install directly into <prefix>/bin —
#    everything belongs under versions/<semver>/.
file(GLOB _flat_bin "${_prefix}/bin/*")
if(_flat_bin)
    message(FATAL_ERROR
        "R-VER-004 violation: flat install detected at '${_prefix}/bin' (${_flat_bin}); "
        "installs MUST stage under versions/<semver>/")
endif()

# 4. The payload binary must be present under versions/<semver>/bin/ (name is
#    platform-dependent: context-hello[.exe]).
file(GLOB _payload "${_versioned}/bin/context-hello*")
if(NOT _payload)
    message(FATAL_ERROR
        "R-VER-004 violation: expected payload under '${_versioned}/bin/' (context-hello*) not found")
endif()

message(STATUS
    "R-VER-004 OK: staged into ${CONTEXT_INSTALL_SUBDIR}/ (payload=${_payload}); no flat install")
