"""Tests for the tools/*-source.json third-party SOURCE pins and their mirror lists (issue #359).

Context-Engine#359: a configure-time third-party fetch with exactly ONE source is a CI supply-line
single point of failure with rollup-wide blast radius — `src/packages/ui/text/` sits in nearly every
leg's dependency closure, so when download.savannah.gnu.org returned 502 for ~an hour (2026-07-22)
it reddened seven unrelated jobs across all three OSes at CMake configure time.

The fix is source REDUNDANCY with the verification untouched: `cmake/ContextDownload.cmake` tries
each pinned source in order and checks the SAME SHA-256 pin after every attempt, so a mirror serving
different bytes is refused and skipped, and the fetch still fails CLOSED (R-SEC-009).

That leaves ONE silent-rot mode, which is what this module guards: a mirror list that is present but
DEAD. Nobody notices a stale fallback while the primary works — it only shows up during the very
outage it was added for. So these checks are offline, deterministic structural assertions over the
committed pins (no network — a networked test would reintroduce exactly the flakiness being fixed):

* every from-source pin declares at least one mirror (the standing no-single-sourced-fetch ratchet);
* every mirror URL carries the PINNED version, so a version bump cannot leave a mirror pointing at
  the previous tarball;
* mirrors are distinct, are not a restatement of the primary, and at least one lives on a DIFFERENT
  host (a "mirror" on the primary's own host fails in the same outage — it is not redundancy);
* a content-addressed mirror URL embedding /sha512/<hex>/ agrees with the pin's declared `sha512`;
* the CMake module that consumes each pin actually FORWARDS the mirrors to context_download (a pin
  that lists mirrors a module never passes is a fallback that can never fire).

Per R-QA-013 each rule is covered happy-path, edge, and failure-path: `validate_pin` is exercised
against synthetic pins that violate one rule at a time (proving every check can actually fail —
a rule that cannot be made to fail is not evidence), then over the live committed pins.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from urllib.parse import urlparse

import pytest
from conftest import TOOLS_DIR

REPO_ROOT = TOOLS_DIR.parent
CMAKE_DIR = REPO_ROOT / "cmake"

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHA512_RE = re.compile(r"^[0-9a-f]{128}$")
SHA512_IN_URL_RE = re.compile(r"/sha512/([0-9a-f]{128})/")

#: Pin stem -> the CMake module that reads it. Both must forward the pin's mirrors to
#: context_download, or the list is decorative.
PIN_CONSUMERS = {
    "freetype-source": CMAKE_DIR / "ContextFreetype.cmake",
    "harfbuzz-source": CMAKE_DIR / "ContextHarfBuzz.cmake",
}

#: Escape hatch for a pin with no byte-identical mirror in existence. Empty on purpose: an entry
#: here is a reviewed, justified exception, never a default. The value is the recorded reason.
NO_MIRROR_JUSTIFIED: dict[str, str] = {}


def source_pins() -> list[Path]:
    """Every committed from-source third-party pin (tools/*-source.json)."""
    return sorted(REPO_ROOT.glob("tools/*-source.json"))


def validate_pin(pin: dict, *, name: str = "<pin>") -> list[str]:
    """Return the list of rule violations in `pin` (empty list == valid).

    Returning findings rather than raising lets one call report every problem in a pin at once,
    and lets the failure-path tests below assert on the specific rule that fired.
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

    if url is not None and version is not None and version not in url:
        problems.append(
            f"{name}: primary 'url' does not contain the pinned version {version!r} — a "
            f"half-finished version bump: {url}")

    mirrors = pin.get("mirrors", [])
    if not isinstance(mirrors, list) or not all(isinstance(m, str) for m in mirrors):
        problems.append(f"{name}: 'mirrors' must be a list of URL strings")
        return problems

    if not mirrors:
        problems.append(
            f"{name}: no 'mirrors' declared — a configure-time fetch with a single source can red "
            f"the entire CI rollup when that one host is down (issue #359). Add a mirror whose "
            f"SHA-256 you verified equals 'sha256', or record a justified exception in "
            f"NO_MIRROR_JUSTIFIED.")
        return problems

    for mirror in mirrors:
        if not mirror.startswith("https://"):
            problems.append(f"{name}: mirror is not an https:// URL: {mirror}")
            continue
        if version is not None and version not in mirror:
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
        primary_host = urlparse(url).hostname
        mirror_hosts = {urlparse(m).hostname for m in mirrors if m.startswith("https://")}
        if mirror_hosts and mirror_hosts <= {primary_host}:
            problems.append(
                f"{name}: every mirror is on the primary's own host ({primary_host}) — that is not "
                f"redundancy, it fails in the same outage. Add a mirror on independent "
                f"infrastructure.")

    return problems


# --------------------------------------------------------------------------------------------
# Happy path
# --------------------------------------------------------------------------------------------

GOOD_PIN = {
    "version": "1.2.3",
    "url": "https://upstream.example/releases/thing-1.2.3.tar.gz",
    "mirrors": [
        "https://mirror.example/pub/thing-1.2.3.tar.gz",
        "https://other.example/thing-1.2.3.tar.gz",
    ],
    "sha256": "a" * 64,
}


def test_valid_pin_has_no_findings():
    assert validate_pin(dict(GOOD_PIN)) == []


def test_valid_pin_with_content_addressed_mirror():
    pin = dict(GOOD_PIN)
    pin["sha512"] = "b" * 128
    pin["mirrors"] = [f"https://lookaside.example/thing-1.2.3.tar.gz/sha512/{'b' * 128}/x-1.2.3.tgz"]
    assert validate_pin(pin) == []


# --------------------------------------------------------------------------------------------
# Failure paths — one rule violated at a time, so each check is proven able to fail
# --------------------------------------------------------------------------------------------

def test_missing_mirrors_is_reported():
    pin = dict(GOOD_PIN)
    pin.pop("mirrors")
    assert any("no 'mirrors' declared" in p for p in validate_pin(pin))


def test_empty_mirrors_is_reported():
    pin = dict(GOOD_PIN, mirrors=[])
    assert any("no 'mirrors' declared" in p for p in validate_pin(pin))


def test_stale_mirror_after_version_bump_is_reported():
    """The silent-rot mode: version bumped, primary re-pointed, mirror forgotten."""
    pin = dict(GOOD_PIN, version="1.2.4")
    pin["url"] = "https://upstream.example/releases/thing-1.2.4.tar.gz"
    findings = validate_pin(pin)
    assert sum("does not contain the pinned version" in p for p in findings) == 2


def test_half_bumped_primary_url_is_reported():
    pin = dict(GOOD_PIN, version="9.9.9")
    assert any("primary 'url' does not contain the pinned version" in p for p in validate_pin(pin))


def test_mirror_on_the_primary_host_only_is_reported():
    pin = dict(GOOD_PIN)
    pin["mirrors"] = ["https://upstream.example/alt/thing-1.2.3.tar.gz"]
    assert any("not redundancy" in p for p in validate_pin(pin))


def test_primary_repeated_in_mirrors_is_reported():
    pin = dict(GOOD_PIN)
    pin["mirrors"] = [GOOD_PIN["url"], "https://mirror.example/pub/thing-1.2.3.tar.gz"]
    assert any("repeated in 'mirrors'" in p for p in validate_pin(pin))


def test_duplicate_mirrors_are_reported():
    pin = dict(GOOD_PIN)
    pin["mirrors"] = ["https://mirror.example/pub/thing-1.2.3.tar.gz"] * 2
    assert any("duplicate entries" in p for p in validate_pin(pin))


def test_non_https_mirror_is_reported():
    pin = dict(GOOD_PIN)
    pin["mirrors"] = ["http://mirror.example/pub/thing-1.2.3.tar.gz"]
    assert any("not an https:// URL" in p for p in validate_pin(pin))


def test_sha512_url_without_declared_sha512_is_reported():
    pin = dict(GOOD_PIN)
    pin["mirrors"] = [f"https://lookaside.example/x/sha512/{'b' * 128}/thing-1.2.3.tar.gz"]
    assert any("declares no 'sha512'" in p for p in validate_pin(pin))


def test_sha512_url_disagreeing_with_declared_sha512_is_reported():
    pin = dict(GOOD_PIN, sha512="b" * 128)
    pin["mirrors"] = [f"https://lookaside.example/x/sha512/{'c' * 128}/thing-1.2.3.tar.gz"]
    assert any("embedded sha512 does not match" in p for p in validate_pin(pin))


def test_bad_sha256_is_reported():
    assert any("'sha256' must be 64" in p for p in validate_pin(dict(GOOD_PIN, sha256="deadbeef")))


def test_bad_sha512_is_reported():
    assert any("'sha512', when declared" in p for p in validate_pin(dict(GOOD_PIN, sha512="nope")))


def test_mirrors_not_a_list_is_reported():
    pin = dict(GOOD_PIN, mirrors="https://mirror.example/pub/thing-1.2.3.tar.gz")
    assert any("must be a list of URL strings" in p for p in validate_pin(pin))


# --------------------------------------------------------------------------------------------
# Integration — the LIVE committed pins
# --------------------------------------------------------------------------------------------

def test_source_pins_exist():
    """Guards the glob itself: a rename that emptied it would make every check below vacuous."""
    stems = {p.stem for p in source_pins()}
    assert {"freetype-source", "harfbuzz-source"} <= stems, stems


@pytest.mark.parametrize("pin_path", source_pins(), ids=lambda p: p.stem)
def test_live_pin_is_valid(pin_path: Path):
    pin = json.loads(pin_path.read_text(encoding="utf-8"))
    if pin_path.stem in NO_MIRROR_JUSTIFIED:
        pytest.skip(f"justified single-source exception: {NO_MIRROR_JUSTIFIED[pin_path.stem]}")
    assert validate_pin(pin, name=pin_path.name) == []


@pytest.mark.parametrize("pin_path", source_pins(), ids=lambda p: p.stem)
def test_live_pin_documents_its_mirrors(pin_path: Path):
    """A mirror list is only maintainable if the bump rule + verification duty are written down."""
    pin = json.loads(pin_path.read_text(encoding="utf-8"))
    assert "$comment_mirrors" in pin, (
        f"{pin_path.name}: declare a '$comment_mirrors' recording how each mirror was verified "
        f"byte-identical and what a version bump must re-point")


@pytest.mark.parametrize("stem,module", sorted(PIN_CONSUMERS.items()))
def test_consumer_module_forwards_mirrors(stem: str, module: Path):
    """A pin may list mirrors that its CMake module never passes — a fallback that cannot fire.

    This is the wiring half of the guard: the pin data and the fetch call must stay coupled.
    """
    text = module.read_text(encoding="utf-8")
    assert "context_download_pin_mirrors" in text, (
        f"{module.name}: does not read {stem}.json's 'mirrors' array")
    assert re.search(r"URLS\s+\$\{_\w+_mirrors\}", text), (
        f"{module.name}: reads the mirrors but never forwards them to context_download(URLS ...)")
