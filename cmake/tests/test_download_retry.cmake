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
#        mirror-less caller is unchanged), and a pin WITH mirrors parses them in declaration order;
#     11. empty URLS — the mirror-less PRODUCTION call shape (`URLS ${<empty>}`), a different
#        cmake_parse_arguments path from omitting URLS entirely.
#   Out of process (cases 12-14, via test_download_retry_child.cmake — behaviours invisible from
#   inside the calling process: the FATAL_ERROR branch every real caller takes, and assertions about
#   attempts NOT made / delays NOT inherited):
#     12. the production fail-closed FATAL message + source de-duplication + per-source backoff reset;
#     13. a mistyped keyword is rejected instead of silently dropping a mirror list;
#     14. a source repeating the SAME wrong bytes is abandoned early, and the good mirror still lands.
#   context_download_from_pin (the pin-driven entry point every pinned caller should use):
#     15. it reads the pin's `mirrors` and FORWARDS them — proven by an unreachable primary in the
#        pin still landing off the pin's mirror, so dropping the forward reds this behaviourally;
#     16. a pin with no "mirrors" key behaves exactly as a single-source fetch did before #359.
# BASE_DELAY 0 keeps the in-process cases instant; only case 12 sleeps (2s), by design.
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

# 11. Mirror-less PRODUCTION call shape — `URLS ${<empty>}` leaves the URLS keyword immediately
#     followed by PATH, which is a DIFFERENT cmake_parse_arguments path from omitting URLS entirely
#     (cases 1-3). It is exactly what ContextFreetype/ContextHarfBuzz emit for a pin with no
#     "mirrors" key, so it is the shape the next mirror-less pin will take. Reuses case 9's parse.
context_download(
    URL "${_good_url}"
    URLS ${_no_mirrors}
    PATH "${WORK_DIR}/empty_urls.bin"
    EXPECTED_SHA256 "${_good_sha}"
    DESCRIPTION "selfcheck-empty-urls"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_empty)
assert_rc("${_rc_empty}" 0 "empty-URLS case")
assert_staged("${WORK_DIR}/empty_urls.bin" "${_good_sha}" "empty-URLS case")

# ------------------------------------------------------------------------------------------------
# Cases 12-14 run OUT OF PROCESS. Three behaviours cannot be observed from inside the calling
# process: the production FATAL_ERROR branch (real callers omit RESULT_VARIABLE, and a FATAL_ERROR
# would abort this script), and any assertion about attempts NOT made or delays NOT inherited. The
# child script exercises them and this script asserts over its exit code + captured output.
# ------------------------------------------------------------------------------------------------
set(_test_dir "${CMAKE_CURRENT_LIST_DIR}")

function(run_child _mode _out_rc _out_log)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -D "CONTEXT_DOWNLOAD_MODULE=${CONTEXT_DOWNLOAD_MODULE}"
            -D "WORK_DIR=${WORK_DIR}/child-${_mode}"
            -D "MODE=${_mode}"
            ${ARGN}
            -P "${_test_dir}/test_download_retry_child.cmake"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    set(${_out_rc} "${_rc}" PARENT_SCOPE)
    # STATUS goes to stdout and WARNING/FATAL_ERROR to stderr; assertions should not care which.
    # Collapse all whitespace: CMake REFLOWS message(WARNING)/message(FATAL_ERROR) text to its own
    # width and re-indents it, so a phrase that fits on one line here can arrive split across two
    # ("...from all 2\n    pinned source(s)..."). Asserting on the raw capture would make every
    # multi-word needle depend on CMake's wrap column rather than on the behaviour under test.
    string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_out}${_err}")
    set(${_out_log} "${_flat}" PARENT_SCOPE)
endfunction()

# Literal substring assertions (not regex) so URLs and "source(s)" need no escaping.
function(assert_log_has _log _needle _label)
    string(FIND "${_log}" "${_needle}" _pos)
    if(_pos LESS 0)
        message(FATAL_ERROR
            "test_download_retry: ${_label} — expected output to contain '${_needle}'.\n${_log}")
    endif()
endfunction()

function(assert_log_lacks _log _needle _label)
    string(FIND "${_log}" "${_needle}" _pos)
    if(NOT _pos LESS 0)
        message(FATAL_ERROR
            "test_download_retry: ${_label} — output must NOT contain '${_needle}'.\n${_log}")
    endif()
endfunction()

# 12. Production fail-closed path + de-duplication + per-source backoff RESET.
#     A caller that omits RESULT_VARIABLE gets message(FATAL_ERROR) — the branch every real caller
#     takes and the only diagnostic anyone gets during a real outage, so its content is asserted.
#     The primary is repeated inside URLS, so the reported source count proves de-duplication ran
#     (2, not 3). RETRIES 2 + BASE_DELAY 1 makes each source emit exactly ONE "retrying in <n>s":
#     with the per-source reset both read 1s; hoisting `set(_delay ...)` out of the source loop
#     would make the second read 3s, so this reds on that regression without timing anything.
run_child(fatal _rc_fatal _log_fatal)
if(_rc_fatal EQUAL 0)
    message(FATAL_ERROR "test_download_retry: fatal case exited 0 — the FATAL_ERROR branch did not fire")
endif()
assert_log_has("${_log_fatal}" "from all 2 pinned source(s)" "fatal case (de-duplication)")
assert_log_has("${_log_fatal}" "R-SEC-009 fail-closed" "fatal case (refusal is attributed)")
assert_log_has("${_log_fatal}" "Per-source reason:" "fatal case (self-contained diagnosis)")
assert_log_has("${_log_fatal}" "dead-a.bin" "fatal case (primary named in the reason log)")
assert_log_has("${_log_fatal}" "dead-b.bin" "fatal case (mirror named in the reason log)")
assert_log_lacks("${_log_fatal}" "retrying in 3s" "fatal case (backoff reset per source)")
string(REGEX MATCHALL "retrying in 1s" _resets "${_log_fatal}")
list(LENGTH _resets _reset_count)
if(NOT _reset_count EQUAL 2)
    message(FATAL_ERROR
        "test_download_retry: fatal case — expected both sources to restart the backoff at 1s, saw "
        "${_reset_count} such retries.\n${_log_fatal}")
endif()

# 13. Mistyped keyword — a silently dropped mirror list is the "fallback that can never fire" mode
#     the pin guard exists to prevent, but no guard covers the non-pin call sites (src/render, the
#     spikes). cmake_parse_arguments swallows an unknown keyword, so context_download rejects it.
run_child(badarg _rc_badarg _log_badarg)
if(_rc_badarg EQUAL 0)
    message(FATAL_ERROR "test_download_retry: badarg case exited 0 — a mistyped keyword was accepted")
endif()
assert_log_has("${_log_badarg}" "unrecognized argument(s): MIRRORS" "badarg case")

# 14. A source serving the SAME wrong bytes every time is abandoned rather than retried to
#     exhaustion (a repeated-identical hash is a deterministic wrong artifact, not a truncation).
#     RETRIES 8: attempt 2 must appear (the repeat was seen) and attempt 3 must NOT (the remaining
#     budget was dropped), while the good mirror still lands — an early abandon, never a failure.
run_child(repeated-mismatch _rc_repeat _log_repeat
    -D "WRONG_URL=${_wrong_url}"
    -D "GOOD_URL=${_good_url}"
    -D "GOOD_SHA=${_good_sha}")
assert_rc("${_rc_repeat}" 0 "repeated-mismatch case")
assert_log_has("${_log_repeat}" "attempt 2/8" "repeated-mismatch case (the repeat was detected)")
assert_log_lacks("${_log_repeat}" "attempt 3/8" "repeated-mismatch case (budget abandoned)")

# 15. context_download_from_pin — the pin-driven entry point, which reads url + sha256 + mirrors out
#     of the pin FILE so a call site cannot forget to forward the mirror list. Asserted
#     BEHAVIOURALLY rather than by linting the consuming module's text: the pin's primary is
#     unreachable and the fetch still lands, which can only happen if the pin's `mirrors` array was
#     read AND passed through. Stop forwarding it and this reds.
set(_pin_file "${WORK_DIR}/thing-source.json")
file(WRITE "${_pin_file}"
    "{\n"
    "  \"version\": \"1.0\",\n"
    "  \"url\": \"${_unreachable}\",\n"
    "  \"mirrors\": [\"${_good_url}\"],\n"
    "  \"sha256\": \"${_good_sha}\"\n"
    "}\n")
context_download_from_pin(
    PIN         "${_pin_file}"
    PATH        "${WORK_DIR}/from_pin.bin"
    DESCRIPTION "selfcheck-from-pin"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_pin
    SOURCE_VARIABLE _src_pin)
assert_rc("${_rc_pin}" 0 "from-pin case")
assert_staged("${WORK_DIR}/from_pin.bin" "${_good_sha}" "from-pin case")
assert_source("${_src_pin}" "${_good_url}" "from-pin case")

# 16. context_download_from_pin over a MIRROR-LESS pin — no "mirrors" key at all, which must behave
#     exactly as a single-source fetch did before #359 rather than erroring on the missing key.
set(_pin_bare "${WORK_DIR}/bare-source.json")
file(WRITE "${_pin_bare}"
    "{\n"
    "  \"version\": \"1.0\",\n"
    "  \"url\": \"${_good_url}\",\n"
    "  \"sha256\": \"${_good_sha}\"\n"
    "}\n")
context_download_from_pin(
    PIN         "${_pin_bare}"
    PATH        "${WORK_DIR}/from_bare_pin.bin"
    DESCRIPTION "selfcheck-from-pin-no-mirrors"
    RETRIES 2 BASE_DELAY 0
    RESULT_VARIABLE _rc_bare)
assert_rc("${_rc_bare}" 0 "from-pin mirror-less case")
assert_staged("${WORK_DIR}/from_bare_pin.bin" "${_good_sha}" "from-pin mirror-less case")

message(STATUS
    "test_download_retry: all 16 cases passed (success / fail-closed / transient / fallback / "
    "skip-bad-mirror / all-sources-bad / primary-preference / urls-only / pin-no-mirrors / "
    "pin-mirrors-in-order / empty-URLS / fatal+dedup+backoff-reset / badarg / repeated-mismatch / "
    "from-pin / from-pin-no-mirrors)")
