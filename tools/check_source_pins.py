#!/usr/bin/env python3
"""Structural gate over the tools/*-source.json third-party SOURCE pins and their mirror lists (#359).

Context-Engine#359: a configure-time third-party fetch with exactly ONE source is a CI supply-line
single point of failure with rollup-wide blast radius — `src/packages/ui/text/` sits in nearly every
leg's dependency closure, so when download.savannah.gnu.org returned 502 for ~an hour (2026-07-22)
it reddened seven unrelated jobs across all three OSes at CMake configure time.

The fix is source REDUNDANCY with the verification untouched; `cmake/ContextDownload.cmake`'s header
is the canonical statement of why that is safe. Consumers call `context_download_from_pin(PIN ...)`,
which reads `url`/`sha256`/`mirrors` out of the pin itself, so a call site CANNOT drop the mirror
list — that wiring is structural and needs no lint here (the CMake self-check proves it
behaviourally, by fetching through a pin whose primary is unreachable).

What is left for this gate is the pin DATA, which nothing else can check:

* every from-source pin declares at least one mirror (the standing no-single-sourced-fetch ratchet);
* every mirror URL names the PINNED version, so a version bump cannot leave a mirror pointing at the
  previous tarball — the rot mode a reviewer is least likely to spot;
* mirrors are distinct, are not a restatement of the primary, and at least one sits on a different
  registrable DOMAIN (a "mirror" on the primary's own infrastructure fails in the same outage);
* a content-addressed mirror URL embedding /sha512/<hex>/ agrees with the pin's declared `sha512`;
* the mirror list is documented, and the pin is actually referenced by a CMake module.

WHAT IT DOES NOT COVER: a mirror that is structurally perfect and simply 404s — a retired SourceForge
project path, a rotated Fedora lookaside — passes forever, and is discovered during the very outage
it was added for. Nothing offline can catch that, and a networked check would reintroduce exactly the
flakiness being removed. The mitigation is the `$comment_mirrors` block each pin carries, recording
that every mirror was fetched and SHA-compared before being listed and that a bump must re-verify.

Run standalone: `python3 tools/check_source_pins.py` (exit 0 clean, 1 findings, 2 gate misconfigured).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import urlparse

from _ci_common import load_json_or_exit

REPO_ROOT = Path(__file__).resolve().parents[1]
CMAKE_DIR = REPO_ROOT / "cmake"
TAG = "source-pins"

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHA512_RE = re.compile(r"^[0-9a-f]{128}$")
SHA512_IN_URL_RE = re.compile(r"/sha512/([0-9a-f]{128})/")

#: Escape hatch for a pin with no byte-identical mirror in existence. Empty on purpose: an entry
#: here is a reviewed, justified exception, never a default. The value is the recorded reason.
#: It waives ONLY the "declare a mirror" rule — every other rule still applies to that pin.
NO_MIRROR_JUSTIFIED: dict[str, str] = {}


def source_pins() -> list[Path]:
    """Every committed from-source third-party pin (tools/*-source.json)."""
    return sorted(REPO_ROOT.glob("tools/*-source.json"))


def version_in_url(version: str, url: str) -> bool:
    """True when `url` names exactly `version` — not merely a version that `version` prefixes.

    A bare `version in url` substring test passes "11.2.1" against an 11.2.10 URL, so the stale
    mirror it exists to catch would sail through on the one bump shape most likely to produce it.
    The match must therefore not be followed by a further version digit, directly (11.2.10) or after
    a dot (11.2.1.4), nor preceded by one (2.13.3 must not match inside 12.13.3). A trailing dot
    that begins a file extension is fine and is the common shape: freetype-2.13.3.tar.gz.
    """
    return re.search(rf"(?<![\d.]){re.escape(version)}(?!\d)(?!\.\d)", url) is not None


def registrable_domain(host: str | None) -> str | None:
    """The last two labels of `host`, as a coarse stand-in for its registrable domain.

    Comparing full hostnames would read `download-mirror.savannah.gnu.org` as independent of
    `download.savannah.gnu.org`, which the FreeType pin's own note contradicts: savannah's download
    host is a redirect DISPATCHER onto its own mirror pool, so a sibling hostname is correlated with
    the very outage being mitigated. Two labels over-merges under multi-label public suffixes
    (a.co.uk), but it errs toward declaring infrastructure SHARED — the safe direction for a
    redundancy check — and dependencies here are deny-by-default, so there is no publicsuffix
    library to do it properly.
    """
    if not host:
        return None
    labels = host.lower().split(".")
    return ".".join(labels[-2:]) if len(labels) >= 2 else host.lower()


def validate_pin(pin: dict, *, name: str = "<pin>", require_mirrors: bool = True) -> list[str]:
    """Return the list of rule violations in `pin` (empty list == valid).

    Returning findings rather than raising lets one call report every problem in a pin at once,
    and lets the failure-path tests assert on the specific rule that fired.

    `require_mirrors=False` waives ONLY the "declare a mirror" rule (the NO_MIRROR_JUSTIFIED
    exception). Every other rule — sha256 shape, https, version coherence — still applies: a
    justified single source is not a justification for an unchecked pin.
    """
    problems: list[str] = []

    version = pin.get("version")
    if not isinstance(version, str) or not version:
        problems.append(f"{name}: 'version' must be a non-empty string")
        version = None

    url = pin.get("url")
    if not isinstance(url, str) or not url.startswith("https://"):
        problems.append(f"{name}: 'url' must be an https:// string")
        url = None

    sha256 = pin.get("sha256")
    if not isinstance(sha256, str) or not SHA256_RE.match(sha256):
        problems.append(f"{name}: 'sha256' must be 64 lowercase hex chars (the authoritative pin)")

    sha512 = pin.get("sha512")
    if sha512 is not None and (not isinstance(sha512, str) or not SHA512_RE.match(sha512)):
        problems.append(f"{name}: 'sha512', when declared, must be 128 lowercase hex chars")
        sha512 = None

    if url is not None and version is not None and not version_in_url(version, url):
        problems.append(
            f"{name}: primary 'url' does not contain the pinned version {version!r} — a "
            f"half-finished version bump: {url}")

    mirrors = pin.get("mirrors", [])
    if not isinstance(mirrors, list) or not all(isinstance(m, str) for m in mirrors):
        problems.append(f"{name}: 'mirrors' must be a list of URL strings")
        return problems

    if not mirrors:
        # The documentation rule below is moot with no mirror list to document, so it stops here.
        if require_mirrors:
            problems.append(
                f"{name}: no 'mirrors' declared — a configure-time fetch with a single source can "
                f"red the entire CI rollup when that one host is down (issue #359). Add a mirror "
                f"whose SHA-256 you verified equals 'sha256', or record a justified exception in "
                f"NO_MIRROR_JUSTIFIED.")
        return problems

    # A mirror list is only maintainable if the verification duty and the bump rule are written
    # down — and that duty is the ONLY thing standing behind a mirror's liveness (see the module
    # docstring), so an empty or placeholder note is not documentation.
    comment = pin.get("$comment_mirrors")
    if not isinstance(comment, str) or len(comment.strip()) < 40:
        problems.append(
            f"{name}: declare a substantive '$comment_mirrors' recording how each mirror was "
            f"verified byte-identical and what a version bump must re-point")

    for mirror in mirrors:
        if not mirror.startswith("https://"):
            problems.append(f"{name}: mirror is not an https:// URL: {mirror}")
            continue
        if ";" in mirror:
            # context_download receives the mirrors as a CMake list, where ';' is the separator: an
            # embedded one would silently split this entry into two bogus sources.
            problems.append(f"{name}: mirror contains ';', which splits a CMake list: {mirror}")
        if version is not None and not version_in_url(version, mirror):
            problems.append(
                f"{name}: mirror does not contain the pinned version {version!r} — it still points "
                f"at an older artifact and would fail the pin if it were ever reached: {mirror}")
        embedded = SHA512_IN_URL_RE.search(mirror)
        if embedded:
            if sha512 is None:
                problems.append(
                    f"{name}: mirror embeds a /sha512/ path segment but the pin declares no "
                    f"'sha512' to check it against: {mirror}")
            elif embedded.group(1) != sha512:
                problems.append(
                    f"{name}: mirror's embedded sha512 does not match the pin's declared 'sha512' "
                    f"— the content-addressed URL would 404: {mirror}")

    if url is not None and url in mirrors:
        problems.append(
            f"{name}: the primary 'url' is repeated in 'mirrors' — it is already tried first, and "
            f"a duplicate only burns a second retry budget on the same host")

    if len(set(mirrors)) != len(mirrors):
        problems.append(f"{name}: 'mirrors' contains duplicate entries")

    if url is not None:
        primary_domain = registrable_domain(urlparse(url).hostname)
        mirror_domains = {
            registrable_domain(urlparse(m).hostname) for m in mirrors if m.startswith("https://")}
        if mirror_domains and mirror_domains <= {primary_domain}:
            problems.append(
                f"{name}: every mirror is on the primary's own infrastructure ({primary_domain}) — "
                f"that is not redundancy, it fails in the same outage. Add a mirror on an "
                f"independent domain.")

    return problems


def unreferenced_pins(pins: list[Path], cmake_dir: Path) -> list[str]:
    """Report any pin no CMake module names — dead pin data that no longer gates anything.

    Deliberately a filename search, not a parse of the fetch call: since consumers pass the pin FILE
    to context_download_from_pin, "the module forwards the mirrors" is no longer a thing a call site
    can get wrong, so the only remaining question is whether the pin is wired up at all.
    """
    try:
        modules = {p: p.read_text(encoding="utf-8") for p in sorted(cmake_dir.glob("*.cmake"))}
    except OSError as exc:
        return [f"cannot read {cmake_dir}: {exc}"]
    return [
        f"{pin.name}: no module under {cmake_dir.name}/ references it — an orphaned pin gates nothing"
        for pin in pins
        if not any(pin.name in text for text in modules.values())
    ]


def main() -> int:
    pins = source_pins()
    if not pins:
        print(
            f"[{TAG}] ERROR: no tools/*-source.json pins found — a rename that emptied the glob "
            f"would make every check below vacuous (a gate that cannot find its subject is not a "
            f"gate)",
            file=sys.stderr,
        )
        return 2

    problems: list[str] = []
    for pin_path in pins:
        pin = load_json_or_exit(pin_path, tag=TAG)
        justified = pin_path.stem in NO_MIRROR_JUSTIFIED
        problems += validate_pin(pin, name=pin_path.name, require_mirrors=not justified)
    problems += unreferenced_pins(pins, CMAKE_DIR)

    if problems:
        for problem in problems:
            print(f"[{TAG}] FINDING: {problem}", file=sys.stderr)
        print(
            f"[{TAG}] FAIL: {len(problems)} problem(s) across {len(pins)} source pin(s) — a "
            f"configure-time fetch must not be single-sourced, and its mirrors must stay current "
            f"(issue #359).",
            file=sys.stderr,
        )
        return 1

    print(
        f"[{TAG}] PASS: {len(pins)} source pin(s) declare current, distinct, independently hosted "
        f"and documented mirrors.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
