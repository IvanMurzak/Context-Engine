# cmake/ContextDownload.cmake — resilient, fail-closed prebuilt downloader (Context-Engine#129).
#
# context_download() wraps file(DOWNLOAD ...) in a bounded retry-with-exponential-backoff loop
# and re-verifies the pinned SHA-256 AFTER every attempt, so a single transient upstream outage
# (an apt.llvm.org / GitHub-releases / CDN 504 or timeout, or a truncated transfer) no longer
# hard-fails a configure. This is RESILIENCE, not a pin change: the SHA-256 pin is still checked
# on every attempt and the function is FAIL-CLOSED — it never leaves an unverified artifact in
# place, and by default it FATAL_ERRORs after exhausting its retries (R-SEC-009). A wrong PIN
# still fails (after the retries add latency only), because a mismatch is verified, not trusted.
#
# Why re-verify instead of leaning on file(DOWNLOAD)'s own EXPECTED_HASH: EXPECTED_HASH aborts
# the whole configure on a mismatch, which makes a TRUNCATED (transient) transfer indistinguish-
# able from a tampered one and unrecoverable. Downloading with STATUS + a separate file(SHA256)
# lets us retry a truncated/short read while STILL refusing a genuinely wrong artifact.
#
# Usage:
#   context_download(
#     URL             <url>                # the remote artifact
#     PATH            <dest-file>          # where to write it
#     EXPECTED_SHA256 <hex>               # the pin (lowercase or uppercase; compared case-insensitively)
#     [DESCRIPTION    <human label>]      # for log/error messages
#     [RETRIES        <n>]                # total attempts (default 4)
#     [BASE_DELAY     <seconds>]          # first backoff, tripled each retry (default 3)
#     [RESULT_VARIABLE <var>])           # if given, set 0 (ok) / 1 (failed) in the caller's scope
#                                         # instead of FATAL_ERROR — production callers omit it
#                                         # (fail-closed); the self-check test uses it.

if(COMMAND context_download)
    return()
endif()

function(context_download)
    set(_opts)
    set(_one URL PATH EXPECTED_SHA256 DESCRIPTION RETRIES BASE_DELAY RESULT_VARIABLE)
    set(_multi)
    cmake_parse_arguments(CD "${_opts}" "${_one}" "${_multi}" ${ARGN})

    if(NOT CD_URL OR NOT CD_PATH OR NOT CD_EXPECTED_SHA256)
        message(FATAL_ERROR "context_download: URL, PATH, and EXPECTED_SHA256 are required")
    endif()
    if(NOT CD_RETRIES)
        set(CD_RETRIES 4)
    endif()
    if(NOT DEFINED CD_BASE_DELAY)
        set(CD_BASE_DELAY 3)
    endif()
    if(NOT CD_DESCRIPTION)
        set(CD_DESCRIPTION "${CD_URL}")
    endif()

    string(TOLOWER "${CD_EXPECTED_SHA256}" _want_sha)

    set(_delay ${CD_BASE_DELAY})
    set(_attempt 1)
    while(_attempt LESS_EQUAL ${CD_RETRIES})
        # Remove any partial/previous artifact so a failed attempt never leaves a stale file.
        file(REMOVE "${CD_PATH}")
        # INACTIVITY_TIMEOUT bounds a STALLED transfer (a connection that opens then goes silent,
        # e.g. a half-dead CDN edge) so it surfaces as a retryable STATUS failure instead of hanging
        # the configure until the outer CI job timeout kills it. Mirrors the Python fetchers'
        # urlopen(timeout=60). Deliberately NOT a total TIMEOUT: a legitimately slow-but-progressing
        # large artifact must not be converted into a failure — only a genuine stall is aborted.
        file(DOWNLOAD "${CD_URL}" "${CD_PATH}" INACTIVITY_TIMEOUT 60 STATUS _dl_status)
        list(GET _dl_status 0 _dl_code)

        if(_dl_code EQUAL 0)
            file(SHA256 "${CD_PATH}" _actual_sha)
            string(TOLOWER "${_actual_sha}" _actual_sha)
            if(_actual_sha STREQUAL _want_sha)
                message(STATUS
                    "context_download: verified ${CD_DESCRIPTION} (SHA-256 OK, attempt ${_attempt})")
                if(CD_RESULT_VARIABLE)
                    set(${CD_RESULT_VARIABLE} 0 PARENT_SCOPE)
                endif()
                return()
            endif()
            set(_why "SHA-256 mismatch (expected ${_want_sha}, got ${_actual_sha})")
        else()
            list(GET _dl_status 1 _dl_msg)
            set(_why "download failed (${_dl_msg})")
        endif()

        if(_attempt LESS ${CD_RETRIES})
            message(WARNING
                "context_download: ${CD_DESCRIPTION} attempt ${_attempt}/${CD_RETRIES} — ${_why}; "
                "retrying in ${_delay}s")
            if(_delay GREATER 0)
                execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep "${_delay}")
            endif()
            math(EXPR _delay "${_delay} * 3")
        else()
            message(WARNING
                "context_download: ${CD_DESCRIPTION} attempt ${_attempt}/${CD_RETRIES} — ${_why}")
        endif()
        math(EXPR _attempt "${_attempt} + 1")
    endwhile()

    # Exhausted every attempt without a verified artifact — never leave one behind (fail-closed).
    file(REMOVE "${CD_PATH}")
    if(CD_RESULT_VARIABLE)
        set(${CD_RESULT_VARIABLE} 1 PARENT_SCOPE)
        return()
    endif()
    message(FATAL_ERROR
        "context_download: FAILED to fetch + verify ${CD_DESCRIPTION} from ${CD_URL} after "
        "${CD_RETRIES} attempts (R-SEC-009 fail-closed — refusing to build against an unverified "
        "artifact). Last reason: ${_why}")
endfunction()
