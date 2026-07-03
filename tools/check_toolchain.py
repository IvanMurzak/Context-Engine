#!/usr/bin/env python3
"""L-42 pinned-toolchain manifest gate for Context Engine CI.

cmake/toolchain-versions.json is the DECLARED, versioned toolchain artifact: per build
target it states the L-42 end-state ("l42_target"), what CI actually uses today
("actual"), and — where one exists — an exact major.minor version pin ("pin"). This
script is the machine half; CI (the .github/actions/pinned-toolchain composite action)
uses it to:

  --print-pin            print the pinned major.minor (e.g. "20.1") for a target
  --print-install-major  print the install major (e.g. "20") used to drive apt.llvm.org
  --verify [TEXT|-]      check a compiler's `--version` output against the pin
  --describe             print the declared target-vs-actual reality for a target

Enforcement levels (per manifest entry):
  strict     -- version mismatch fails CI (exit 1). Linux/clang today.
  advisory   -- mismatch prints a GitHub ::warning:: annotation, exit 0 (drift is
                VISIBLE but does not break the build). macOS/Apple clang today.
  documented -- no live verification defined; --verify on such an entry is a
                configuration error (exit 2). Windows/MSVC and web/emscripten today.

Exit codes: 0 = OK; 1 = strict verification failure; 2 = configuration/usage error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from _ci_common import load_json_or_exit

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "cmake" / "toolchain-versions.json"

# First dotted-numeric token in a compiler's --version output, e.g.
#   "Ubuntu clang version 20.1.8 (...)"                          -> 20.1.8
#   "Apple clang version 17.0.0 (clang-1700.0.13.3)"             -> 17.0.0
#   "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211" -> 19.44.35211
VERSION_RE = re.compile(r"\b(\d+(?:\.\d+)+)\b")

VALID_ENFORCEMENTS = {"strict", "advisory", "documented"}


def fail_config(message: str) -> "NoReturn":  # noqa: F821 - py3.9-friendly annotation
    print(f"[toolchain] ERROR: {message}", file=sys.stderr)
    sys.exit(2)


def load_manifest(path: Path) -> dict:
    manifest = load_json_or_exit(path, tag="toolchain")
    if "targets" not in manifest or not isinstance(manifest["targets"], dict):
        fail_config(f"manifest {path} has no 'targets' table")
    return manifest


def entry_for(manifest: dict, platform: str) -> dict:
    entry = manifest["targets"].get(platform)
    if entry is None:
        known = ", ".join(sorted(manifest["targets"]))
        fail_config(f"unknown platform '{platform}' (manifest knows: {known})")
    enforcement = entry.get("enforcement")
    if enforcement not in VALID_ENFORCEMENTS:
        fail_config(f"platform '{platform}' has invalid enforcement '{enforcement}'")
    return entry


def extract_version(text: str) -> str | None:
    """Return the first dotted-numeric version token in compiler output, or None."""
    match = VERSION_RE.search(text)
    return match.group(1) if match else None


def version_matches(actual: str, pin: str) -> bool:
    """Component-wise prefix match: pin '20.1' matches '20.1.8' but not '20.10.1'."""
    actual_parts = actual.split(".")
    pin_parts = pin.split(".")
    if len(actual_parts) < len(pin_parts):
        return False
    return actual_parts[: len(pin_parts)] == pin_parts


def verify(entry: dict, platform: str, version_output: str) -> int:
    enforcement = entry["enforcement"]
    pin = entry.get("pin")
    if enforcement == "documented" or not pin:
        fail_config(
            f"platform '{platform}' is '{enforcement}' with no version pin — "
            "--verify is not defined for it (see the manifest entry's notes)"
        )
    actual = extract_version(version_output)
    if actual is None:
        fail_config(f"could not find a version number in compiler output: {version_output!r}")
    if version_matches(actual, pin):
        print(f"[toolchain] OK: {platform} compiler {actual} matches pin {pin}")
        return 0
    if enforcement == "strict":
        print(
            f"[toolchain] FAIL: {platform} compiler is {actual}, manifest pins {pin} "
            f"(strict — update cmake/toolchain-versions.json or fix the install)",
            file=sys.stderr,
        )
        return 1
    # advisory: visible, not breaking
    print(
        f"::warning title=Toolchain drift ({platform})::compiler is {actual}, manifest "
        f"records {pin} — update cmake/toolchain-versions.json (advisory, not failing CI)"
    )
    return 0


def describe(entry: dict, platform: str) -> None:
    pin = entry.get("pin") or "(none)"
    print(f"[toolchain] {platform} ({entry['enforcement']})")
    print(f"[toolchain]   L-42 target: {entry.get('l42_target', '(unspecified)')}")
    print(f"[toolchain]   actual:      {entry.get('actual', '(unspecified)')}")
    print(f"[toolchain]   pin:         {pin}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--platform", required=True, help="manifest target key, e.g. linux-x86_64")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--print-pin", action="store_true")
    mode.add_argument("--print-install-major", action="store_true")
    mode.add_argument(
        "--verify",
        nargs="?",
        const="-",
        metavar="TEXT",
        help="compiler --version output to check ('-' or omitted = read stdin)",
    )
    mode.add_argument("--describe", action="store_true")
    args = parser.parse_args(argv)

    manifest = load_manifest(args.manifest)
    entry = entry_for(manifest, args.platform)

    if args.print_pin:
        pin = entry.get("pin")
        if not pin:
            fail_config(f"platform '{args.platform}' has no version pin")
        print(pin)
        return 0

    if args.print_install_major:
        major = entry.get("install", {}).get("major")
        if major is None:
            fail_config(f"platform '{args.platform}' has no install.major")
        print(major)
        return 0

    if args.verify is not None:
        text = sys.stdin.read() if args.verify == "-" else args.verify
        return verify(entry, args.platform, text)

    describe(entry, args.platform)
    return 0


if __name__ == "__main__":
    sys.exit(main())
