# cmake/ContextDownload.cmake — resilient, fail-closed prebuilt downloader (Context-Engine#129, #359).
#
# context_download() fetches a pinned artifact from ONE OR MORE interchangeable sources. Each source
# is wrapped in a bounded retry-with-exponential-backoff loop, and the pinned SHA-256 is re-verified
# AFTER every attempt; when a source is exhausted the function FALLS BACK to the next one. So neither
# a single transient upstream outage (an apt.llvm.org / GitHub-releases / CDN 504 or timeout, or a
# truncated transfer) NOR a sustained outage of one HOST can hard-fail a configure.
#
# This is RESILIENCE, not a pin change: the SHA-256 pin is still checked on every attempt of every
# source, and the function is FAIL-CLOSED — it never leaves an unverified artifact in place, and by
# default it FATAL_ERRORs after exhausting every source (R-SEC-009). A wrong PIN still fails (the
# retries add latency only), because a mismatch is verified, not trusted.
#
# Why multiple sources are SAFE here (issue #359): the pin is what makes a mirror trustworthy. A
# mirror is never "believed" — its bytes are hashed and compared against the SAME pin as the primary,
# so a mirror serving different content is REFUSED and skipped exactly like a corrupt transfer. The
# security property is unchanged; only availability improves. Correspondingly a mirror must serve the
# BYTE-IDENTICAL artifact (verify before pinning it: fetch it and compare its SHA-256), and a mirror
# on the same host/CDN as the primary is not redundancy at all — it fails in the same outage.
#
# Why re-verify instead of leaning on file(DOWNLOAD)'s own EXPECTED_HASH: EXPECTED_HASH aborts
# the whole configure on a mismatch, which makes a TRUNCATED (transient) transfer indistinguish-
# able from a tampered one and unrecoverable. Downloading with STATUS + a separate file(SHA256)
# lets us retry a truncated/short read, fall through to the next mirror, and STILL refuse a
# genuinely wrong artifact.
#
# Usage:
#   context_download(
#     URL             <url>                # the primary source (the upstream home); tried FIRST
#     [URLS           <url> ...]           # ordered fallback mirrors, tried after URL, same pin
#     PATH            <dest-file>          # where to write it
#     EXPECTED_SHA256 <hex>               # the pin (lowercase or uppercase; compared case-insensitively)
#     [DESCRIPTION    <human label>]      # for log/error messages
#     [RETRIES        <n>]                # attempts PER SOURCE (default 4)
#     [BASE_DELAY     <seconds>]          # first backoff, tripled each retry, reset per source (default 3)
#     [RESULT_VARIABLE <var>]            # if given, set 0 (ok) / 1 (failed) in the caller's scope
#                                         # instead of FATAL_ERROR — production callers omit it
#                                         # (fail-closed); the self-check test uses it.
#     [SOURCE_VARIABLE <var>])           # if given, set to the URL that actually verified (empty
#                                         # when none did). Makes WHICH source served observable
#                                         # rather than merely inferable from the log — the
#                                         # self-check asserts fall-through and primary-preference
#                                         # through it, so those cases can genuinely fail.
#
#   At least one of URL / URLS is required. Worst-case latency on a total outage is
#   <source-count> x <RETRIES> attempts, which is the price of not redding a whole CI rollup.
#
# context_download_pin_mirrors() reads the OPTIONAL top-level "mirrors" array of a tools/*.json pin
# file, so a pin stays the SINGLE source of truth for its own mirror list (no URL duplicated in CMake).

if(COMMAND context_download)
    return()
endif()

# context_download_pin_mirrors(<pin-json-string> <out-var>)
#   Parse the optional top-level "mirrors": [ ... ] array of a pin file into a CMake list suitable for
#   `context_download(... URLS ${<out-var>})`. A pin with no "mirrors" key (or an empty one) yields an
#   empty list, so a caller needs no conditional — a mirror-less pin simply behaves as before.
function(context_download_pin_mirrors _pin_json _out_var)
    set(_mirrors)
    string(JSON _count ERROR_VARIABLE _json_err LENGTH "${_pin_json}" mirrors)
    if(NOT _json_err AND _count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_i RANGE ${_last})
            string(JSON _mirror GET "${_pin_json}" mirrors ${_i})
            list(APPEND _mirrors "${_mirror}")
        endforeach()
    endif()
    set(${_out_var} "${_mirrors}" PARENT_SCOPE)
endfunction()

function(context_download)
    set(_opts)
    set(_one URL PATH EXPECTED_SHA256 DESCRIPTION RETRIES BASE_DELAY RESULT_VARIABLE
             SOURCE_VARIABLE)
    set(_multi URLS)
    cmake_parse_arguments(CD "${_opts}" "${_one}" "${_multi}" ${ARGN})

    # Ordered source list: the primary (URL) first, then the pinned mirrors (URLS). De-duplicated so a
    # pin that repeats its primary inside "mirrors" does not spend a second retry budget on it.
    set(_sources)
    if(CD_URL)
        list(APPEND _sources "${CD_URL}")
    endif()
    if(CD_URLS)
        list(APPEND _sources ${CD_URLS})
    endif()
    if(_sources)
        list(REMOVE_DUPLICATES _sources)
    endif()

    if(NOT _sources OR NOT CD_PATH OR NOT CD_EXPECTED_SHA256)
        message(FATAL_ERROR
            "context_download: URL (or URLS), PATH, and EXPECTED_SHA256 are required")
    endif()
    if(NOT CD_RETRIES)
        set(CD_RETRIES 4)
    endif()
    if(NOT DEFINED CD_BASE_DELAY)
        set(CD_BASE_DELAY 3)
    endif()
    if(NOT CD_DESCRIPTION)
        list(GET _sources 0 CD_DESCRIPTION)
    endif()

    string(TOLOWER "${CD_EXPECTED_SHA256}" _want_sha)
    list(LENGTH _sources _source_count)
    set(_source_index 0)
    if(CD_SOURCE_VARIABLE)
        set(${CD_SOURCE_VARIABLE} "" PARENT_SCOPE)
    endif()

    foreach(_url IN LISTS _sources)
        math(EXPR _source_index "${_source_index} + 1")
        # The backoff schedule is per-source: a fresh mirror must not inherit the previous one's
        # already-tripled delay, or the last mirror in a long list would sleep for minutes.
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
            file(DOWNLOAD "${_url}" "${CD_PATH}" INACTIVITY_TIMEOUT 60 STATUS _dl_status)
            list(GET _dl_status 0 _dl_code)

            if(_dl_code EQUAL 0)
                file(SHA256 "${CD_PATH}" _actual_sha)
                string(TOLOWER "${_actual_sha}" _actual_sha)
                if(_actual_sha STREQUAL _want_sha)
                    message(STATUS
                        "context_download: verified ${CD_DESCRIPTION} (SHA-256 OK, source "
                        "${_source_index}/${_source_count}, attempt ${_attempt}): ${_url}")
                    if(CD_RESULT_VARIABLE)
                        set(${CD_RESULT_VARIABLE} 0 PARENT_SCOPE)
                    endif()
                    if(CD_SOURCE_VARIABLE)
                        set(${CD_SOURCE_VARIABLE} "${_url}" PARENT_SCOPE)
                    endif()
                    return()
                endif()
                # A mismatch is either a truncated/short read (retryable) or a source serving the
                # WRONG artifact (never accepted — we move on to the next pinned source instead).
                set(_why "SHA-256 mismatch (expected ${_want_sha}, got ${_actual_sha})")
            else()
                list(GET _dl_status 1 _dl_msg)
                set(_why "download failed (${_dl_msg})")
            endif()

            if(_attempt LESS ${CD_RETRIES})
                message(WARNING
                    "context_download: ${CD_DESCRIPTION} source ${_source_index}/${_source_count} "
                    "attempt ${_attempt}/${CD_RETRIES} — ${_why}; retrying in ${_delay}s")
                if(_delay GREATER 0)
                    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep "${_delay}")
                endif()
                math(EXPR _delay "${_delay} * 3")
            else()
                message(WARNING
                    "context_download: ${CD_DESCRIPTION} source ${_source_index}/${_source_count} "
                    "attempt ${_attempt}/${CD_RETRIES} — ${_why}")
            endif()
            math(EXPR _attempt "${_attempt} + 1")
        endwhile()

        if(_source_index LESS _source_count)
            message(WARNING
                "context_download: ${CD_DESCRIPTION} source ${_source_index}/${_source_count} "
                "(${_url}) exhausted after ${CD_RETRIES} attempts — falling back to the next "
                "pinned source (same SHA-256 pin)")
        endif()
    endforeach()

    # Every source exhausted without a verified artifact — never leave one behind (fail-closed).
    file(REMOVE "${CD_PATH}")
    string(REPLACE ";" ", " _source_list "${_sources}")
    if(CD_RESULT_VARIABLE)
        set(${CD_RESULT_VARIABLE} 1 PARENT_SCOPE)
        return()
    endif()
    message(FATAL_ERROR
        "context_download: FAILED to fetch + verify ${CD_DESCRIPTION} from all ${_source_count} "
        "pinned source(s) [${_source_list}], ${CD_RETRIES} attempts each (R-SEC-009 fail-closed — "
        "refusing to build against an unverified artifact). Last reason: ${_why}")
endfunction()
