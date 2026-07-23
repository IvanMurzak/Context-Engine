#!/usr/bin/env bash
# Retry a command with exponential backoff for transient CI network failures (Context-Engine#129).
#
# CI fetches many external prebuilts fresh every run (apt.llvm.org clang, emsdk, apt packages);
# a single upstream 504/timeout must not hard-fail an otherwise-green build. Wrap such a fetch:
#
#   bash tools/ci-retry.sh -- <command> [args...]
#   bash tools/ci-retry.sh -n 5 -d 10 -- bash -c 'sudo apt-get update && sudo apt-get install -y X'
#
# Options (before the `--`):
#   -n <attempts>    total attempts (default 4)
#   -d <base_delay>  first backoff in seconds, tripled each retry (default 5)
#   -t <timeout>     per-attempt hard ceiling in seconds via GNU `timeout` (default 0 = unbounded).
#
# CI toolchain-resilience fix (apt.llvm.org "accepts the connection, then goes silent" incident):
# a STALL is fundamentally different from a fast FAILURE (a 504, connection refused) — only a
# bounded exit code is retryable, so a hung wrapped command never reaches the retry logic below at
# all. `-t` converts a stall into a retryable failure by wrapping each attempt in `timeout`, which
# (run without --foreground) puts the wrapped command in its own process group and signals the
# WHOLE group on expiry, so a hang inside a `sudo`-spawned grandchild (e.g. `sudo llvm.sh`'s own
# internal apt-get) is reaped too, not just the immediate child. `-k 10` escalates to SIGKILL 10s
# after the initial TERM for a child that ignores it. This is layered UNDER any timeout the wrapped
# command sets on itself (e.g. wget's own --timeout/--tries) — belt and suspenders: the inner bound
# catches the common case cheaply, the outer `-t` is the backstop that catches everything else
# (apt operations, DNS hangs, anything with no timeout flag of its own).
#
# Exit status is the command's own on eventual success, or 1 after exhausting every attempt.
# This is RESILIENCE only — the command it wraps still enforces its own SHA/pin verification.
set -uo pipefail

attempts=4
delay=5
per_attempt_timeout=0
while [ $# -gt 0 ]; do
    case "$1" in
        -n) attempts="$2"; shift 2 ;;
        -d) delay="$2"; shift 2 ;;
        -t) per_attempt_timeout="$2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done

if [ $# -eq 0 ]; then
    echo "ci-retry: no command given (usage: ci-retry.sh [-n attempts] [-d base_delay] [-t per_attempt_timeout] -- cmd...)" >&2
    exit 2
fi

run_attempt() {
    if [ "$per_attempt_timeout" -gt 0 ] && command -v timeout >/dev/null 2>&1; then
        timeout -k 10 "$per_attempt_timeout" "$@"
        return $?
    fi
    if [ "$per_attempt_timeout" -gt 0 ]; then
        echo "ci-retry: -t $per_attempt_timeout requested but no 'timeout' binary on PATH; running unbounded" >&2
    fi
    "$@"
}

attempt=1
while :; do
    # Capture the exit code from the command itself, NOT from the `if`: bash resets `$?` to 0
    # after a no-`else` `if` block whose condition was false, so `rc=$?` placed after `if
    # run_attempt; then ...; fi` would never see the real (non-zero) exit code — it has to be
    # read immediately after run_attempt runs, before anything else executes.
    run_attempt "$@"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        exit 0
    fi
    timed_out=0
    [ "$rc" -eq 124 ] && [ "$per_attempt_timeout" -gt 0 ] && timed_out=1

    if [ "$attempt" -ge "$attempts" ]; then
        if [ "$timed_out" -eq 1 ]; then
            echo "ci-retry: '$*' timed out after ${per_attempt_timeout}s on every attempt (exhausted $attempts attempts)" >&2
        else
            echo "ci-retry: '$*' failed after $attempts attempts" >&2
        fi
        exit 1
    fi
    if [ "$timed_out" -eq 1 ]; then
        echo "ci-retry: attempt $attempt/$attempts of '$*' TIMED OUT after ${per_attempt_timeout}s; retrying in ${delay}s" >&2
    else
        echo "ci-retry: attempt $attempt/$attempts of '$*' failed; retrying in ${delay}s" >&2
    fi
    sleep "$delay"
    delay=$(( delay * 3 ))
    attempt=$(( attempt + 1 ))
done
