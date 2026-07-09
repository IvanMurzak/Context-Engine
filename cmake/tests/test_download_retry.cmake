# Self-check for cmake/ContextDownload.cmake's context_download() (Context-Engine#129).
#
# Run under `cmake -P` (registered as the `download-retry-selfcheck` ctest). Exercises, offline
# via file:// URLs so it runs on the local GCC dev gate AND every CI leg:
#   1. success  — correct pin verifies and stages the artifact (RESULT 0);
#   2. fail-closed — a WRONG pin is refused after the retries, artifact removed (RESULT 1);
#   3. transient — an unreachable URL is retried then reported failed (RESULT 1).
# BASE_DELAY 0 keeps it instant (no real backoff sleeps).
cmake_minimum_required(VERSION 3.25)

foreach(_req CONTEXT_DOWNLOAD_MODULE FIXTURE_DIR WORK_DIR)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR "test_download_retry: -D${_req}=<path> is required")
    endif()
endforeach()

include("${CONTEXT_DOWNLOAD_MODULE}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Build a file:// URL for the fixture that is valid on both POSIX and Windows.
set(_fixture "${FIXTURE_DIR}/payload.bin")
if(NOT EXISTS "${_fixture}")
    message(FATAL_ERROR "test_download_retry: fixture missing: ${_fixture}")
endif()
file(SHA256 "${_fixture}" _good_sha)
file(TO_CMAKE_PATH "${_fixture}" _fixture_cmake)
if(_fixture_cmake MATCHES "^/")
    set(_good_url "file://${_fixture_cmake}")        # POSIX: /abs -> file:///abs
else()
    set(_good_url "file:///${_fixture_cmake}")       # Windows: C:/abs -> file:///C:/abs
endif()

# 1. Success — correct pin.
context_download(
    URL "${_good_url}"
    PATH "${WORK_DIR}/ok.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-success"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_ok)
if(NOT _rc_ok EQUAL 0)
    message(FATAL_ERROR "test_download_retry: success case returned rc=${_rc_ok} (expected 0)")
endif()
if(NOT EXISTS "${WORK_DIR}/ok.bin")
    message(FATAL_ERROR "test_download_retry: success case did not stage the artifact")
endif()
file(SHA256 "${WORK_DIR}/ok.bin" _staged_sha)
if(NOT _staged_sha STREQUAL _good_sha)
    message(FATAL_ERROR "test_download_retry: staged artifact SHA differs from the fixture")
endif()

# 2. Fail-closed — wrong pin is refused; the artifact must NOT be left behind.
set(_bad_sha "0000000000000000000000000000000000000000000000000000000000000000")
context_download(
    URL "${_good_url}"
    PATH "${WORK_DIR}/badhash.bin"
    EXPECTED_SHA256 "${_bad_sha}"
    DESCRIPTION "selfcheck-wrong-pin"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_bad)
if(NOT _rc_bad EQUAL 1)
    message(FATAL_ERROR "test_download_retry: wrong-pin case returned rc=${_rc_bad} (expected 1)")
endif()
if(EXISTS "${WORK_DIR}/badhash.bin")
    message(FATAL_ERROR "test_download_retry: wrong-pin case left an unverified artifact behind")
endif()

# 3. Transient — an unreachable URL is retried then reported failed (not a hard configure abort).
context_download(
    URL "file:///no/such/context-download/missing.bin"
    PATH "${WORK_DIR}/missing.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-unreachable"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_missing)
if(NOT _rc_missing EQUAL 1)
    message(FATAL_ERROR "test_download_retry: unreachable case returned rc=${_rc_missing} (expected 1)")
endif()

message(STATUS "test_download_retry: all 3 cases passed (success / fail-closed / transient)")
