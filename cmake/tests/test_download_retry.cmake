# Self-check for cmake/ContextDownload.cmake's context_download() (Context-Engine#129, #359).
#
# Run under `cmake -P` (registered as the `download-retry-selfcheck` ctest). Exercises, offline
# via file:// URLs so it runs on the local GCC dev gate AND every CI leg:
#   Single-source (issue #129 — the retry/verify contract):
#     1. success  — correct pin verifies and stages the artifact (RESULT 0);
#     2. fail-closed — a WRONG pin is refused after the retries, artifact removed (RESULT 1);
#     3. transient — an unreachable URL is retried then reported failed (RESULT 1).
#   Multi-source (issue #359 — the mirror fallback; a fallback that is never exercised is not a
#   fallback, so each of these asserts the fall-through actually happened, not merely that the
#   call returned 0):
#     4. fallback — an unreachable PRIMARY falls through to a good mirror (RESULT 0, real bytes);
#     5. skip-then-land — a mirror serving DIFFERENT bytes is skipped, a later good mirror lands;
#     6. fail-closed across mirrors — a wrong-bytes mirror is NEVER accepted; when it is the only
#        reachable source the fetch still refuses and leaves nothing behind (RESULT 1);
#     7. primary preference — a good primary wins and a later wrong-bytes mirror is never consulted
#        (the staged bytes are the primary's, so a bad mirror cannot overwrite a verified artifact);
#     8. URLS-only — the mirror list alone (no URL) is a valid source list;
#     9/10. context_download_pin_mirrors — a pin with NO "mirrors" key parses to an empty list (so a
#        mirror-less caller is unchanged), and a pin WITH mirrors parses them in declaration order.
# BASE_DELAY 0 keeps it instant (no real backoff sleeps).
cmake_minimum_required(VERSION 3.25)

foreach(_req CONTEXT_DOWNLOAD_MODULE FIXTURE_DIR WORK_DIR)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR "test_download_retry: -D${_req}=<path> is required")
    endif()
endforeach()

include("${CONTEXT_DOWNLOAD_MODULE}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Build a file:// URL for a local file that is valid on both POSIX and Windows.
function(path_url _path _out_url _out_sha)
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "test_download_retry: source file missing: ${_path}")
    endif()
    file(SHA256 "${_path}" _sha)
    file(TO_CMAKE_PATH "${_path}" _cmake_path)
    if(_cmake_path MATCHES "^/")
        set(${_out_url} "file://${_cmake_path}" PARENT_SCOPE)   # POSIX: /abs -> file:///abs
    else()
        set(${_out_url} "file:///${_cmake_path}" PARENT_SCOPE)  # Windows: C:/abs -> file:///C:/abs
    endif()
    set(${_out_sha} "${_sha}" PARENT_SCOPE)
endfunction()

path_url("${FIXTURE_DIR}/payload.bin" _good_url _good_sha)
path_url("${FIXTURE_DIR}/payload-mirror-mismatch.bin" _wrong_url _wrong_sha)

# A byte-IDENTICAL copy of the good payload at a DIFFERENT url. Two sources that BOTH verify are
# what make source ORDER observable at all: with only one verifying source, reversing the list still
# stages the same bytes from the same url, so a primary-preference assertion could never fail.
set(_good_copy "${WORK_DIR}/mirror-copy-of-payload.bin")
file(COPY_FILE "${FIXTURE_DIR}/payload.bin" "${_good_copy}")
path_url("${_good_copy}" _good_copy_url _good_copy_sha)
if(NOT _good_copy_sha STREQUAL _good_sha)
    message(FATAL_ERROR "test_download_retry: the byte-identical copy is not byte-identical")
endif()
if(_good_copy_url STREQUAL _good_url)
    message(FATAL_ERROR
        "test_download_retry: the copy resolved to the same url — the primary-preference case "
        "would be vacuous")
endif()
if(_good_sha STREQUAL _wrong_sha)
    message(FATAL_ERROR
        "test_download_retry: the two fixtures hash identically — the wrong-bytes-mirror cases "
        "below would be vacuous")
endif()
set(_unreachable "file:///no/such/context-download/missing.bin")

# assert_staged(<path> <expected-sha> <label>) — the artifact exists AND carries the expected bytes.
# Asserting the BYTES (not just existence) is what makes cases 4/5/7 non-vacuous: it distinguishes
# "a source was used" from "the RIGHT source was used".
function(assert_staged _path _want_sha _label)
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "test_download_retry: ${_label} did not stage the artifact")
    endif()
    file(SHA256 "${_path}" _got)
    if(NOT _got STREQUAL _want_sha)
        message(FATAL_ERROR
            "test_download_retry: ${_label} staged the WRONG bytes (expected ${_want_sha}, got ${_got})")
    endif()
endfunction()

function(assert_absent _path _label)
    if(EXISTS "${_path}")
        message(FATAL_ERROR "test_download_retry: ${_label} left an unverified artifact behind")
    endif()
endfunction()

function(assert_rc _rc _want _label)
    if(NOT _rc EQUAL ${_want})
        message(FATAL_ERROR "test_download_retry: ${_label} returned rc=${_rc} (expected ${_want})")
    endif()
endfunction()

# assert_source(<reported-url> <expected-url> <label>) — WHICH source served.
# Without this, order is unobservable: because every source is verified against the same pin, the
# staged BYTES are identical whichever one wins, so a fall-through case and a primary-preference
# case cannot be told apart by content alone (a reversed source order would leave both green). The
# SOURCE_VARIABLE out-param is what makes cases 4/5/7 able to fail for their OWN reason.
function(assert_source _got _want _label)
    if(NOT _got STREQUAL _want)
        message(FATAL_ERROR
            "test_download_retry: ${_label} was served by ${_got} (expected ${_want})")
    endif()
endfunction()

# 1. Success — correct pin.
context_download(
    URL "${_good_url}"
    PATH "${WORK_DIR}/ok.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-success"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_ok)
assert_rc("${_rc_ok}" 0 "success case")
assert_staged("${WORK_DIR}/ok.bin" "${_good_sha}" "success case")

# 2. Fail-closed — wrong pin is refused; the artifact must NOT be left behind.
set(_bad_sha "0000000000000000000000000000000000000000000000000000000000000000")
context_download(
    URL "${_good_url}"
    PATH "${WORK_DIR}/badhash.bin"
    EXPECTED_SHA256 "${_bad_sha}"
    DESCRIPTION "selfcheck-wrong-pin"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_bad)
assert_rc("${_rc_bad}" 1 "wrong-pin case")
assert_absent("${WORK_DIR}/badhash.bin" "wrong-pin case")

# 3. Transient — an unreachable URL is retried then reported failed (not a hard configure abort).
context_download(
    URL "${_unreachable}"
    PATH "${WORK_DIR}/missing.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-unreachable"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_missing)
assert_rc("${_rc_missing}" 1 "unreachable case")

# 4. Fallback (issue #359) — the PRIMARY is down for the whole retry budget and the fetch still
#    succeeds off a mirror. This is the case the whole issue exists for: a savannah-style outage
#    must no longer red the configure.
context_download(
    URL "${_unreachable}"
    URLS "${_good_url}"
    PATH "${WORK_DIR}/fallback.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-fallback"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_fallback
    SOURCE_VARIABLE _src_fallback)
assert_rc("${_rc_fallback}" 0 "fallback case")
assert_staged("${WORK_DIR}/fallback.bin" "${_good_sha}" "fallback case")
assert_source("${_src_fallback}" "${_good_url}" "fallback case")

# 5. Skip-then-land — a mirror serving DIFFERENT bytes must not be accepted AND must not abort the
#    chain: the next good mirror still lands. (A `return` on the first mismatching mirror would
#    pass case 6 while silently breaking this one.)
context_download(
    URL "${_unreachable}"
    URLS "${_wrong_url}" "${_good_url}"
    PATH "${WORK_DIR}/skip_then_land.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-skip-bad-mirror"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_skip
    SOURCE_VARIABLE _src_skip)
assert_rc("${_rc_skip}" 0 "skip-bad-mirror case")
assert_staged("${WORK_DIR}/skip_then_land.bin" "${_good_sha}" "skip-bad-mirror case")
assert_source("${_src_skip}" "${_good_url}" "skip-bad-mirror case")

# 6. Fail-closed ACROSS mirrors (R-SEC-009 unchanged by the redundancy) — every source either
#    unreachable or serving the wrong bytes ⇒ refuse, and stage nothing. The wrong-bytes mirror
#    downloaded FINE; only the pin rejected it.
context_download(
    URL "${_unreachable}"
    URLS "${_wrong_url}"
    PATH "${WORK_DIR}/all_bad.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-all-sources-bad"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_all_bad)
assert_rc("${_rc_all_bad}" 1 "all-sources-bad case")
assert_absent("${WORK_DIR}/all_bad.bin" "all-sources-bad case")

# 7. Primary preference — sources are tried IN ORDER and the fetch stops at the first that verifies.
#    The second source here is a BYTE-IDENTICAL copy at a different url, so both would satisfy the
#    pin and only the reported SOURCE distinguishes them: reverse the order and this case reds. The
#    trailing wrong-bytes mirror additionally proves a bad entry later in the list is inert once a
#    good source has served.
context_download(
    URL "${_good_url}"
    URLS "${_good_copy_url}" "${_wrong_url}"
    PATH "${WORK_DIR}/primary_first.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-primary-preference"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_primary
    SOURCE_VARIABLE _src_primary)
assert_rc("${_rc_primary}" 0 "primary-preference case")
assert_staged("${WORK_DIR}/primary_first.bin" "${_good_sha}" "primary-preference case")
assert_source("${_src_primary}" "${_good_url}" "primary-preference case")

# 8. URLS-only — a caller may supply the source list entirely through URLS (no URL).
context_download(
    URLS "${_unreachable}" "${_good_url}"
    PATH "${WORK_DIR}/urls_only.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-urls-only"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_urls_only)
assert_rc("${_rc_urls_only}" 0 "urls-only case")
assert_staged("${WORK_DIR}/urls_only.bin" "${_good_sha}" "urls-only case")

# 9. context_download_pin_mirrors — a pin WITHOUT a "mirrors" key yields an empty list, so a
#    mirror-less pin (and every caller that passes no URLS at all: wgpu-native, the spikes) keeps
#    the exact pre-#359 single-source behaviour rather than erroring on the missing key.
context_download_pin_mirrors([[{"version":"1.0","url":"https://x.invalid/a","sha256":"ab"}]] _no_mirrors)
if(NOT _no_mirrors STREQUAL "")
    message(FATAL_ERROR
        "test_download_retry: pin without a 'mirrors' key yielded ${_no_mirrors} (expected empty)")
endif()

# 10. context_download_pin_mirrors — a pin WITH mirrors yields them in declaration order. Order is
#     part of the contract (the primary/first-listed source is preferred), so it is asserted, not
#     just membership.
context_download_pin_mirrors(
    [[{"version":"1.0","url":"https://x.invalid/a","mirrors":["https://m1.invalid/a","https://m2.invalid/a"],"sha256":"ab"}]]
    _two_mirrors)
if(NOT _two_mirrors STREQUAL "https://m1.invalid/a;https://m2.invalid/a")
    message(FATAL_ERROR
        "test_download_retry: pin mirrors parsed as '${_two_mirrors}' (expected the two declared "
        "urls in order)")
endif()

message(STATUS
    "test_download_retry: all 10 cases passed (success / fail-closed / transient / fallback / "
    "skip-bad-mirror / all-sources-bad / primary-preference / urls-only / pin-no-mirrors / "
    "pin-mirrors-in-order)")
