# cmake/ContextDownload.cmake — resilient, fail-closed prebuilt downloader (Context-Engine#129, #359).
#
# context_download() fetches a pinned artifact from ONE OR MORE interchangeable sources. Each source
# is wrapped in a bounded retry-with-exponential-backoff loop, and the pinned SHA-256 is re-verified
# AFTER every attempt; when a source is exhausted the function FALLS BACK to the next one. So neither
# a single transient upstream outage (an apt.llvm.org / GitHub-releases / CDN 504 or timeout, or a
# truncated transfer) NOR a sustained outage of one HOST can hard-fail a configure.
#
# WHY MULTIPLE SOURCES ARE SAFE (issue #359) — the canonical statement of this property; other files
# point HERE rather than restating it. The pin is what makes a mirror trustworthy: a mirror is never
# "believed", its bytes are hashed against the SAME pin as the primary, so a mirror serving different
# content is REFUSED and skipped exactly like a corrupt transfer. The function is FAIL-CLOSED — it
# never leaves an unverified artifact in place, and by default it FATAL_ERRORs after exhausting every
# source (R-SEC-009). A wrong PIN still fails; the retries add latency only. So redundancy is purely
# an availability change, and the author's duty is the corollary: a mirror must serve the
# BYTE-IDENTICAL artifact (fetch it and compare its SHA-256 BEFORE listing it), and must sit on
# independent infrastructure — a mirror on the primary's own host/CDN fails in the same outage.
#
# Why re-verify instead of leaning on file(DOWNLOAD)'s own EXPECTED_HASH: EXPECTED_HASH aborts
# the whole configure on a mismatch, which makes a TRUNCATED (transient) transfer indistinguish-
# able from a tampered one and unrecoverable. Downloading with STATUS + a separate file(SHA256)
# lets us retry a truncated/short read, fall through to the next mirror, and STILL refuse a
# genuinely wrong artifact.
#
# A mismatch that REPEATS BYTE-IDENTICALLY is not a truncation, though: the source is serving a
# deterministically wrong artifact — an HTTP-200 HTML interstitial from a degraded CDN mirror is the
# common shape — and retrying it cannot ever succeed. Such a source is abandoned at once rather than
# burning its whole retry budget plus backoff (4 x <artifact> downloaded and ~39s slept, per source,
# at the defaults) on a fetch that is already decided. Refusal is unchanged; only the wasted latency
# goes, which matters because a stalled configure is the very failure mode #359 exists to remove.
#
# Usage:
#   context_download(
#     URL             <url>                # the primary source (the upstream home); tried FIRST
#     [URLS           <url> ...]           # ordered fallback mirrors, tried after URL, same pin
#     PATH            <dest-file>          # where to write it
#     EXPECTED_SHA256 <hex>               # the pin (lowercase or uppercase; compared case-insensitively)
#     [DESCRIPTION    <human label>]      # for log/error messages
#     [RETRIES        <n>]                # MAX attempts per source (default 4; a source whose bytes
#                                         # mismatch the pin identically twice is abandoned early)
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
# PIN-DRIVEN CALLERS SHOULD NOT CALL context_download DIRECTLY — use context_download_from_pin(),
# which reads url + sha256 + mirrors straight out of a tools/*.json pin. Passing the pieces by hand
# makes forgetting the mirror list a SILENT downgrade back to single-sourced (the fetch still works;
# only the fallback quietly ceases to exist), which is precisely the defect #359 is about. Taking the
# pin file as the argument makes the mirrors structurally unforgettable instead of something a
# separate lint has to police.
#
#   context_download_from_pin(
#     PIN         <tools/*.json>            # the pin: version + url + sha256 + optional mirrors
#     PATH        <dest-file>               # where to write it
#     [DESCRIPTION <human label>]           # defaults to the pin's file name
#     [RETRIES <n>] [BASE_DELAY <s>] [RESULT_VARIABLE <var>] [SOURCE_VARIABLE <var>])
#
# context_download_pin_mirrors() (used by the above) parses the OPTIONAL top-level "mirrors" array of
# a pin, so a pin stays the SINGLE source of truth for its own sources — no URL is duplicated in CMake.

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

# context_download_from_pin(PIN <pin-file> PATH <dest> [DESCRIPTION <s>] [RETRIES <n>]
#                           [BASE_DELAY <s>] [RESULT_VARIABLE <var>] [SOURCE_VARIABLE <var>])
#   Fetch + verify the artifact a tools/*.json pin describes, reading `url`, `sha256` and the
#   optional `mirrors` straight out of it. THE preferred entry point for a pin-driven caller: the
#   mirror list can no longer be forgotten at the call site, so single-sourcing a pinned fetch stops
#   being a silent downgrade and the wiring needs no external lint to police it.
function(context_download_from_pin)
    cmake_parse_arguments(CDP "" "PIN;PATH;DESCRIPTION;RETRIES;BASE_DELAY;RESULT_VARIABLE;SOURCE_VARIABLE" "" ${ARGN})
    if(CDP_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "context_download_from_pin: unrecognized argument(s): ${CDP_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT CDP_PIN OR NOT CDP_PATH)
        message(FATAL_ERROR "context_download_from_pin: PIN and PATH are required")
    endif()
    if(NOT EXISTS "${CDP_PIN}")
        message(FATAL_ERROR "context_download_from_pin: pin file not found at ${CDP_PIN}")
    endif()

    file(READ "${CDP_PIN}" _pin_json)
    string(JSON _pin_url GET "${_pin_json}" url)
    string(JSON _pin_sha GET "${_pin_json}" sha256)
    context_download_pin_mirrors("${_pin_json}" _pin_mirrors)
    if(NOT CDP_DESCRIPTION)
        get_filename_component(CDP_DESCRIPTION "${CDP_PIN}" NAME)
    endif()

    # Only forward the optional knobs the caller actually set, so context_download's own defaults
    # (and its `NOT DEFINED CD_BASE_DELAY` check, for which "unset" and "0" differ) stay intact.
    set(_fwd)
    foreach(_opt RETRIES BASE_DELAY RESULT_VARIABLE SOURCE_VARIABLE)
        if(DEFINED CDP_${_opt})
            list(APPEND _fwd ${_opt} "${CDP_${_opt}}")
        endif()
    endforeach()

    context_download(
        URL             "${_pin_url}"
        URLS            ${_pin_mirrors}
        PATH            "${CDP_PATH}"
        EXPECTED_SHA256 "${_pin_sha}"
        DESCRIPTION     "${CDP_DESCRIPTION}"
        ${_fwd})

    # context_download set these in OUR scope; hoist them one more level to the real caller.
    foreach(_out RESULT_VARIABLE SOURCE_VARIABLE)
        if(CDP_${_out})
            set(${CDP_${_out}} "${${CDP_${_out}}}" PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

function(context_download)
    set(_opts)
    set(_one URL PATH EXPECTED_SHA256 DESCRIPTION RETRIES BASE_DELAY RESULT_VARIABLE
             SOURCE_VARIABLE)
    set(_multi URLS)
    cmake_parse_arguments(CD "${_opts}" "${_one}" "${_multi}" ${ARGN})
    # A MISTYPED keyword (URLS -> MIRRORS, SOURCE_VARIABLE -> SOURCE_VAR) is otherwise swallowed in
    # silence: the mirror list simply never arrives and the fetch fails with a perfectly good
    # fallback sitting unused — the exact "a fallback that can never fire" mode tools/
    # check_source_pins.py guards for the two pin-driven callers, but which no guard covers for the
    # others (src/render, the spikes, and whatever is added next). Fail loudly instead.
    # NOT CD_KEYWORDS_MISSING_VALUES: `URLS ${_empty}` (a mirror-less pin) is the legitimate shape.
    if(CD_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "context_download: unrecognized argument(s): ${CD_UNPARSED_ARGUMENTS}")
    endif()

    # Ordered source list: the primary (URL) first, then the pinned mirrors (URLS). De-duplicated so a
    # pin that repeats its primary inside "mirrors" does not spend a second retry budget on it.
    set(_sources)
    if(CD_URL)
        list(APPEND _sources "${CD_URL}")
    endif()
    if(CD_URLS)
        list(APPEND _sources ${CD_URLS})
    endif()
    list(REMOVE_DUPLICATES _sources)   # a no-op on an empty list; the next check rejects that case

    if(NOT _sources OR NOT CD_PATH OR NOT CD_EXPECTED_SHA256)
        message(FATAL_ERROR
            "context_download: URL (or URLS), PATH, and EXPECTED_SHA256 are required")
    endif()
    # The two idioms differ DELIBERATELY. RETRIES uses the falsy check so `RETRIES 0` is rewritten to
    # the default rather than degenerating into "never attempt anything" (which would skip the loop
    # entirely and report failure with no reason recorded). BASE_DELAY uses NOT DEFINED because
    # `BASE_DELAY 0` is meaningful and load-bearing — it is how the offline self-check runs instantly.
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
    set(_reason_log "")
    if(CD_SOURCE_VARIABLE)
        set(${CD_SOURCE_VARIABLE} "" PARENT_SCOPE)
    endif()

    foreach(_url IN LISTS _sources)
        math(EXPR _source_index "${_source_index} + 1")
        # One label per scope, built where its counters change, so the four message sites below stay
        # in lockstep instead of each re-spelling the same prefix.
        set(_src_tag "${CD_DESCRIPTION} source ${_source_index}/${_source_count}")
        # The backoff schedule is per-source: a fresh mirror must not inherit the previous one's
        # already-tripled delay, or the last mirror in a long list would sleep for minutes.
        set(_delay ${CD_BASE_DELAY})
        # Likewise per-source: the previous attempt's hash, used to spot a source that is serving a
        # deterministically WRONG artifact (see the header). Empty is never a valid SHA-256, so the
        # first attempt can never match it.
        set(_prev_sha "")
        # _attempt counts attempts ALREADY MADE, so it stays exact on both loop exits (budget
        # exhausted, or the early abandon below) and can be reported as-is.
        set(_attempt 0)
        while(_attempt LESS ${CD_RETRIES})
            math(EXPR _attempt "${_attempt} + 1")
            set(_att_tag "${_src_tag} attempt ${_attempt}/${CD_RETRIES}")
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
                # Wrong bytes are never left on disk, not even across the backoff sleep below.
                file(REMOVE "${CD_PATH}")
                # A mismatch is either a truncated/short read (retryable) or a source serving the
                # WRONG artifact (never accepted — we move on to the next pinned source instead).
                # An IDENTICAL hash twice running settles which: a truncation lands at a different
                # length each time, so a repeat means this source is deterministically wrong and no
                # number of further attempts will change that. Abandon it now (see the header).
                # NOTE: set() with several arguments builds a ';'-JOINED LIST, not a concatenated
                # string — _why is operator-facing (it reaches the fatal message), so it is composed
                # once, as one string, and extended with string(APPEND).
                set(_why "SHA-256 mismatch (expected ${_want_sha}, got ${_actual_sha})")
                if(_actual_sha STREQUAL _prev_sha)
                    string(APPEND _why
                        ", repeated identically — this source is serving a deterministically WRONG"
                        " artifact, not a truncated transfer; abandoning it without further retries")
                    message(WARNING "context_download: ${_att_tag} — ${_why}")
                    break()
                endif()
                set(_prev_sha "${_actual_sha}")
            else()
                list(GET _dl_status 1 _dl_msg)
                # Deliberately NOT branched on _dl_code to skip retries on a "permanent" HTTP error:
                # file(DOWNLOAD)'s STATUS collapses every HTTP >= 400 into code 22 with the same
                # message, so a gone-for-good 404 is indistinguishable here from the TRANSIENT 502
                # that motivated #359 — and failing fast on that 502 is exactly what this module
                # exists to prevent. Distinguishing them would mean parsing the LOG output.
                set(_why "download failed (${_dl_msg})")
            endif()

            if(_attempt LESS ${CD_RETRIES})
                # STATUS, not WARNING: a retry that is about to be followed by another attempt is
                # progress, and the mirror fall-through this module exists for is a SUCCESS path —
                # emitting a warning (with CMake's attached call stack) per attempt buried a healthy
                # configure under five of them. The last attempt of a source, and the fall-through
                # itself, stay WARNING because those are the outcomes worth noticing.
                message(STATUS "context_download: ${_att_tag} — ${_why}; retrying in ${_delay}s")
                if(_delay GREATER 0)
                    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep "${_delay}")
                endif()
                math(EXPR _delay "${_delay} * 3")
            else()
                message(WARNING "context_download: ${_att_tag} — ${_why}")
            endif()
        endwhile()

        # One line per source, so the single fatal message below is self-contained: why the PRIMARY
        # failed is what an operator needs, and scrolling back through every preceding attempt to
        # find it is exactly what a fail-closed error should not require.
        string(APPEND _reason_log "\n  - ${_url}: ${_why}")

        if(_source_index LESS _source_count)
            message(WARNING
                "context_download: ${_src_tag} (${_url}) gave up after ${_attempt} attempt(s) — "
                "falling back to the next pinned source (same SHA-256 pin)")
        endif()
    endforeach()

    # Every source exhausted without a verified artifact — never leave one behind (fail-closed).
    file(REMOVE "${CD_PATH}")
    if(CD_RESULT_VARIABLE)
        set(${CD_RESULT_VARIABLE} 1 PARENT_SCOPE)
        return()
    endif()
    message(FATAL_ERROR
        "context_download: FAILED to fetch + verify ${CD_DESCRIPTION} from all ${_source_count} "
        "pinned source(s), up to ${CD_RETRIES} attempts each (R-SEC-009 fail-closed — refusing to "
        "build against an unverified artifact). Per-source reason:${_reason_log}")
endfunction()
