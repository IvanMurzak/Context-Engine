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
#
# Exit status is the command's own on eventual success, or 1 after exhausting every attempt.
# This is RESILIENCE only — the command it wraps still enforces its own SHA/pin verification.
set -uo pipefail

attempts=4
delay=5
while [ $# -gt 0 ]; do
    case "$1" in
        -n) attempts="$2"; shift 2 ;;
        -d) delay="$2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done

if [ $# -eq 0 ]; then
    echo "ci-retry: no command given (usage: ci-retry.sh [-n attempts] [-d base_delay] -- cmd...)" >&2
    exit 2
fi

attempt=1
while :; do
    if "$@"; then
        exit 0
    fi
    if [ "$attempt" -ge "$attempts" ]; then
        echo "ci-retry: '$*' failed after $attempts attempts" >&2
        exit 1
    fi
    echo "ci-retry: attempt $attempt/$attempts of '$*' failed; retrying in ${delay}s" >&2
    sleep "$delay"
    delay=$(( delay * 3 ))
    attempt=$(( attempt + 1 ))
done
