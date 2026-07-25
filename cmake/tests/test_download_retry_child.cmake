# Child script for test_download_retry.cmake's out-of-process cases (Context-Engine#359).
#
# Three of context_download()'s behaviours are unobservable from INSIDE the calling process, so the
# parent self-check runs this script under `cmake -P` and asserts over its captured output + exit
# code instead of over a return value:
#
#   fatal            — the PRODUCTION failure path. Real callers omit RESULT_VARIABLE, so they take
#                      the message(FATAL_ERROR) branch, which no in-process case can reach (a
#                      FATAL_ERROR would abort the self-check itself). This mode also pins the two
#                      things that branch reports — the DE-DUPLICATED source count and the
#                      per-source reason log — and, via the retry lines, the PER-SOURCE BACKOFF
#                      RESET: with the reset, source 2's first retry waits BASE_DELAY again; without
#                      it, it inherits source 1's already-tripled delay. Both are message-content
#                      assertions, so nothing here depends on wall-clock timing.
#   badarg           — the mistyped-keyword guard (a silently dropped mirror list is the failure it
#                      exists to prevent), likewise a FATAL_ERROR.
#   repeated-mismatch — a source returning the SAME wrong bytes every time is abandoned instead of
#                      burning its whole retry budget. Observable only as the attempts NOT made.
#
# -D MODE=<mode> selects one; every mode is offline (file:// or an unreachable file:// path).
cmake_minimum_required(VERSION 3.25)

foreach(_req CONTEXT_DOWNLOAD_MODULE WORK_DIR MODE)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR "test_download_retry_child: -D${_req}=<value> is required")
    endif()
endforeach()

include("${CONTEXT_DOWNLOAD_MODULE}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(_dead_a "file:///no/such/context-download/dead-a.bin")
set(_dead_b "file:///no/such/context-download/dead-b.bin")
set(_sha_placeholder "0000000000000000000000000000000000000000000000000000000000000000")

if(MODE STREQUAL "fatal")
    # The primary is repeated inside URLS: de-duplication must collapse it, so the fatal message
    # reports 2 sources rather than 3. RETRIES 2 + BASE_DELAY 1 makes each source emit exactly one
    # "retrying in <n>s" line; with the per-source reset BOTH say 1s (total sleep: 2s).
    context_download(
        URL "${_dead_a}"
        URLS "${_dead_a}" "${_dead_b}"
        PATH "${WORK_DIR}/fatal.bin"
        EXPECTED_SHA256 "${_sha_placeholder}"
        DESCRIPTION "childcheck-fatal"
        RETRIES 2 BASE_DELAY 1)
    message(FATAL_ERROR "test_download_retry_child: fatal mode returned instead of aborting")

elseif(MODE STREQUAL "badarg")
    # MIRRORS is not a keyword — the good fallback must NOT be silently dropped.
    context_download(
        URL "${_dead_a}"
        MIRRORS "${_dead_b}"
        PATH "${WORK_DIR}/badarg.bin"
        EXPECTED_SHA256 "${_sha_placeholder}"
        DESCRIPTION "childcheck-badarg"
        RETRIES 1 BASE_DELAY 0
        RESULT_VARIABLE _rc)
    message(FATAL_ERROR "test_download_retry_child: badarg mode returned rc=${_rc} instead of aborting")

elseif(MODE STREQUAL "repeated-mismatch")
    if(NOT DEFINED WRONG_URL OR NOT DEFINED GOOD_URL OR NOT DEFINED GOOD_SHA)
        message(FATAL_ERROR "test_download_retry_child: repeated-mismatch needs WRONG_URL/GOOD_URL/GOOD_SHA")
    endif()
    # RETRIES 8 against a source that deterministically serves the wrong bytes: attempt 2 proves the
    # repeat was detected, and the ABSENCE of attempt 3 proves the remaining budget was abandoned.
    # The good mirror must still land, so the early abandon is a fall-through, never a hard failure.
    context_download(
        URL "${WRONG_URL}"
        URLS "${GOOD_URL}"
        PATH "${WORK_DIR}/repeated.bin"
        EXPECTED_SHA256 "${GOOD_SHA}"
        DESCRIPTION "childcheck-repeated-mismatch"
        RETRIES 8 BASE_DELAY 0
        RESULT_VARIABLE _rc
        SOURCE_VARIABLE _src)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "test_download_retry_child: repeated-mismatch did not fall through (rc=${_rc})")
    endif()
    if(NOT _src STREQUAL "${GOOD_URL}")
        message(FATAL_ERROR "test_download_retry_child: repeated-mismatch served by ${_src}")
    endif()
    message(STATUS "test_download_retry_child: repeated-mismatch fell through to the good mirror")

else()
    message(FATAL_ERROR "test_download_retry_child: unknown MODE '${MODE}'")
endif()
