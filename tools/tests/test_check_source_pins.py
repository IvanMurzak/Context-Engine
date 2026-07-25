"""Tests for tools/check_source_pins.py — the #359 no-single-sourced-configure-fetch gate.

The gate's whole value is that it goes RED when a pin rots, so per R-QA-013 every rule is exercised
against a synthetic pin violating exactly that one rule (proving the check can actually fail — a
rule that cannot be made to fail is not evidence), alongside the shapes it must NOT flag. Then the
LIVE committed pins are run through it.

Note what is deliberately NOT tested here: that a consuming CMake module forwards the mirror list.
Consumers pass the pin FILE to context_download_from_pin, so the forward is structural rather than a
call-site obligation, and `cmake/tests/test_download_retry.cmake` case 15 proves it behaviourally by
fetching through a pin whose primary is unreachable. Linting CMake text for it would be both weaker
and redundant.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import load_tool

check_source_pins = load_tool("check_source_pins")

validate_pin = check_source_pins.validate_pin
source_pins = check_source_pins.source_pins
version_in_url = check_source_pins.version_in_url
unreferenced_pins = check_source_pins.unreferenced_pins

COMMENT = "Verified byte-identical on 2026-07-25; a version bump must re-point and re-verify each."

GOOD_PIN = {
    "$comment_mirrors": COMMENT,
    "version": "1.2.3",
    "url": "https://upstream.example/releases/thing-1.2.3.tar.gz",
    "mirrors": [
        "https://mirror.example/pub/thing-1.2.3.tar.gz",
        "https://other.example/thing-1.2.3.tar.gz",
    ],
    "sha256": "a" * 64,
}


# --------------------------------------------------------------------------------------------
# Happy path
# --------------------------------------------------------------------------------------------

def test_valid_pin_has_no_findings():
    assert validate_pin(dict(GOOD_PIN)) == []


def test_valid_pin_with_content_addressed_mirror():
    pin = dict(GOOD_PIN)
    pin["sha512"] = "b" * 128
    pin["mirrors"] = [f"https://lookaside.example/thing-1.2.3.tar.gz/sha512/{'b' * 128}/x-1.2.3.tgz"]
    assert validate_pin(pin) == []


# --------------------------------------------------------------------------------------------
# Failure paths — exactly ONE rule violated per row, so each check is proven able to fail
# --------------------------------------------------------------------------------------------

@pytest.mark.parametrize("overrides,needle", [
    pytest.param({"mirrors": []}, "no 'mirrors' declared", id="empty-mirrors"),
    pytest.param({"mirrors": ["https://upstream.example/alt/thing-1.2.3.tar.gz"]},
                 "not redundancy", id="mirror-on-primary-domain"),
    pytest.param({"mirrors": [GOOD_PIN["url"], "https://mirror.example/pub/thing-1.2.3.tar.gz"]},
                 "repeated in 'mirrors'", id="primary-repeated-in-mirrors"),
    pytest.param({"mirrors": ["https://mirror.example/pub/thing-1.2.3.tar.gz"] * 2},
                 "duplicate entries", id="duplicate-mirrors"),
    pytest.param({"mirrors": ["http://mirror.example/pub/thing-1.2.3.tar.gz"]},
                 "not an https:// URL", id="non-https-mirror"),
    pytest.param({"mirrors": [f"https://lookaside.example/x/sha512/{'b' * 128}/thing-1.2.3.tar.gz"]},
                 "declares no 'sha512'", id="sha512-url-without-declared-sha512"),
    pytest.param({"sha512": "b" * 128,
                  "mirrors": [f"https://lookaside.example/x/sha512/{'c' * 128}/thing-1.2.3.tar.gz"]},
                 "embedded sha512 does not match", id="sha512-url-disagrees"),
    pytest.param({"sha256": "deadbeef"}, "'sha256' must be 64", id="bad-sha256"),
    pytest.param({"sha512": "nope"}, "'sha512', when declared", id="bad-sha512"),
    pytest.param({"version": ""}, "'version' must be a non-empty string", id="empty-version"),
    pytest.param({"url": "http://upstream.example/releases/thing-1.2.3.tar.gz"},
                 "'url' must be an https:// string", id="non-https-primary"),
    pytest.param({"version": "9.9.9"}, "primary 'url' does not contain the pinned version",
                 id="half-bumped-primary"),
    pytest.param({"mirrors": "https://mirror.example/pub/thing-1.2.3.tar.gz"},
                 "must be a list of URL strings", id="mirrors-not-a-list"),
    pytest.param({"mirrors": ["https://mirror.example/pub/thing-1.2.30.tar.gz"]},
                 "does not contain the pinned version", id="mirror-for-a-merely-prefixed-version"),
    pytest.param({"$comment_mirrors": ""}, "substantive '$comment_mirrors'", id="empty-comment"),
    pytest.param({"$comment_mirrors": "mirrors"}, "substantive '$comment_mirrors'",
                 id="placeholder-comment"),
])
def test_one_violated_rule_is_reported(overrides, needle):
    assert any(needle in p for p in validate_pin(dict(GOOD_PIN, **overrides)))


def test_missing_mirrors_key_is_reported():
    pin = dict(GOOD_PIN)
    pin.pop("mirrors")
    assert any("no 'mirrors' declared" in p for p in validate_pin(pin))


def test_missing_comment_mirrors_key_is_reported():
    pin = dict(GOOD_PIN)
    pin.pop("$comment_mirrors")
    assert any("substantive '$comment_mirrors'" in p for p in validate_pin(pin))


def test_stale_mirror_after_version_bump_is_reported():
    """The silent-rot mode: version bumped, primary re-pointed, BOTH mirrors forgotten."""
    pin = dict(GOOD_PIN, version="1.2.4")
    pin["url"] = "https://upstream.example/releases/thing-1.2.4.tar.gz"
    findings = validate_pin(pin)
    assert sum("does not contain the pinned version" in p for p in findings) == 2


def test_mirror_on_a_sibling_hostname_of_the_primary_is_reported():
    """A different HOSTNAME under the same domain is the same infrastructure.

    The FreeType pin's own note makes this concrete: savannah's download host is a redirect
    dispatcher onto its own mirror pool, so `download-mirror.savannah.gnu.org` is correlated with
    `download.savannah.gnu.org` and is not redundancy against the outage that motivated #359.
    """
    pin = dict(GOOD_PIN)
    pin["url"] = "https://download.savannah.gnu.org/releases/thing-1.2.3.tar.gz"
    pin["mirrors"] = ["https://download-mirror.savannah.gnu.org/releases/thing-1.2.3.tar.gz"]
    assert any("not redundancy" in p for p in validate_pin(pin))


def test_mirror_containing_a_semicolon_is_reported():
    """';' is the CMake list separator — an embedded one splits the entry into bogus sources."""
    pin = dict(GOOD_PIN)
    pin["mirrors"] = ["https://mirror.example/pub/thing-1.2.3.tar.gz;evil"]
    assert any("splits a CMake list" in p for p in validate_pin(pin))


# --------------------------------------------------------------------------------------------
# Version matching must be bounded, not a bare substring
# --------------------------------------------------------------------------------------------

@pytest.mark.parametrize("version,url,expected", [
    # The shape every real pin uses: version followed by the file extension.
    ("2.13.3", "https://x.example/freetype-2.13.3.tar.gz", True),
    ("11.2.1", "https://x.example/download/11.2.1/harfbuzz-11.2.1.tar.xz", True),
    # A LATER version that the pinned one merely prefixes must NOT count as a match — this is the
    # stale-mirror case a bare `version in url` test would wave through.
    ("11.2.1", "https://x.example/download/11.2.10/harfbuzz-11.2.10.tar.xz", False),
    ("1.2.3", "https://x.example/thing-1.2.3.4.tar.gz", False),
    # ...nor may it match as a SUFFIX of a longer version.
    ("2.13.3", "https://x.example/thing-12.13.3.tar.gz", False),
])
def test_version_in_url_is_bounded(version, url, expected):
    assert version_in_url(version, url) is expected


# --------------------------------------------------------------------------------------------
# The NO_MIRROR_JUSTIFIED waiver is NARROW
# --------------------------------------------------------------------------------------------

def test_waiver_suppresses_only_the_mirror_rule():
    pin = dict(GOOD_PIN, sha256="deadbeef")
    pin.pop("mirrors")
    findings = validate_pin(pin, require_mirrors=False)
    assert not any("no 'mirrors' declared" in p for p in findings)
    assert any("'sha256' must be 64" in p for p in findings), findings


def test_waiver_does_not_waive_a_broken_primary_url():
    pin = dict(GOOD_PIN, version="9.9.9")
    pin.pop("mirrors")
    findings = validate_pin(pin, require_mirrors=False)
    assert any("primary 'url' does not contain the pinned version" in p for p in findings)


def test_no_mirror_justified_is_empty_by_default():
    """An entry is a reviewed exception; a silently-growing list would hollow out the ratchet."""
    assert check_source_pins.NO_MIRROR_JUSTIFIED == {}


# --------------------------------------------------------------------------------------------
# A pin nothing references is dead data
# --------------------------------------------------------------------------------------------

def test_unreferenced_pin_is_reported(tmp_path):
    cmake_dir = tmp_path / "cmake"
    cmake_dir.mkdir()
    (cmake_dir / "ContextThing.cmake").write_text(
        'context_download_from_pin(PIN "${D}/../tools/wired-source.json" PATH "${P}")\n',
        encoding="utf-8")
    wired = tmp_path / "wired-source.json"
    orphan = tmp_path / "orphan-source.json"
    for pin in (wired, orphan):
        pin.write_text("{}", encoding="utf-8")

    findings = unreferenced_pins([wired, orphan], cmake_dir)
    assert len(findings) == 1, findings
    assert "orphan-source.json" in findings[0]


# --------------------------------------------------------------------------------------------
# Integration — the LIVE committed pins
# --------------------------------------------------------------------------------------------

def test_source_pins_exist():
    """Guards the glob itself: a rename that emptied it would make every check below vacuous."""
    stems = {p.stem for p in source_pins()}
    assert {"freetype-source", "harfbuzz-source"} <= stems, stems


def test_gate_passes_on_the_live_tree(capsys):
    """Runs every rule over the real pins. `main()` prints each finding with the pin name, so a
    failure here is as legible as a per-pin test would be — and it also covers main()'s own wiring."""
    assert check_source_pins.main() == 0, capsys.readouterr().err


def test_gate_reports_a_missing_subject(monkeypatch, tmp_path, capsys):
    """Exit 2, not 0: a gate that cannot find its subject must fail loudly, never pass vacuously."""
    monkeypatch.setattr(check_source_pins, "REPO_ROOT", tmp_path)
    assert check_source_pins.main() == 2
    assert "no tools/*-source.json pins found" in capsys.readouterr().err


def test_live_pins_are_referenced_by_a_cmake_module():
    assert unreferenced_pins(source_pins(), check_source_pins.CMAKE_DIR) == []
