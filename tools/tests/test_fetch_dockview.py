"""Tests for tools/fetch_dockview.py — the pinned dockview-core fetch/verify gate (R-QA-013).

Covers the happy path (offline ``--source``, exercising the real verify + tar member extraction),
every fail-closed path (tarball SHA mismatch, PER-MEMBER SHA mismatch, missing source file, missing
member, unsafe member path, and each malformed-manifest refusal), and idempotency — all on tiny
synthetic npm-style tarballs so no network is touched.

The per-member SHA assertions are the point of this suite: the tarball pin alone cannot catch an
inner-layout swap, so ``fetch_dockview`` verifies EACH extracted file on its own. A final
integration block asserts the REAL tools/dockview-toolchain.json is well-formed and still pinned to
the owner-consented version (08 §3) — a bump past 7.0.2 must re-enter the consent gate, so a silent
version bump fails here by design.
"""

from __future__ import annotations

import hashlib
import io
import json
import tarfile
from pathlib import Path

import pytest
from conftest import load_tool

fetch_dockview = load_tool("fetch_dockview")

VERSION = "9.9.9"
PACKAGE = "dockview-core"
JS_BYTES = b"/* fake dockview-core UMD bundle */\nwindow['dockview-core']={};\n"
CSS_BYTES = b".dv-dockview{--dv-fake:1}\n"

JS_MEMBER = "package/dist/dockview-core.min.js"
CSS_MEMBER = "package/dist/styles/dockview.css"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _make_tgz(members: dict[str, bytes]) -> bytes:
    """Build a synthetic npm tarball carrying the given members (+ an unpinned sibling)."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, data in members.items():
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        # An unpinned sibling proves extraction picks out exactly the pinned members.
        readme = b"# dockview-core\n"
        rinfo = tarfile.TarInfo(name="package/README.md")
        rinfo.size = len(readme)
        tar.addfile(rinfo, io.BytesIO(readme))
    return buf.getvalue()


def _make_source(tmp_path: Path, *, members: dict[str, bytes] | None = None,
                 subdir: str = "source") -> tuple[Path, dict]:
    """Build a synthetic --source dir (one npm tarball) and a matching manifest.

    The member PINS are always computed from the module constants, never from ``members`` — so
    passing a swapped payload yields the tarball-verifies-but-member-does-not case directly, and
    ``subdir`` lets one test build a second, conflicting source tree beside the first.
    """
    members = members if members is not None else {JS_MEMBER: JS_BYTES, CSS_MEMBER: CSS_BYTES}
    source = tmp_path / subdir
    source.mkdir()
    tgz = _make_tgz(members)
    file_name = f"{PACKAGE}-{VERSION}.tgz"
    (source / file_name).write_bytes(tgz)
    manifest = {
        "package": PACKAGE,
        "version": VERSION,
        "license": "MIT",
        "tarball_url_template": "https://example.invalid/{package}-{version}.tgz",
        "tarball_file": file_name,
        "tarball_sha256": _sha(tgz),
        "members": {
            "dockview-core.min.js": {"member": JS_MEMBER, "sha256": _sha(JS_BYTES)},
            "dockview.css": {"member": CSS_MEMBER, "sha256": _sha(CSS_BYTES)},
        },
    }
    return source, manifest


def _write_manifest(tmp_path: Path, manifest: dict) -> Path:
    path = tmp_path / "dockview-toolchain.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


# --------------------------------------------------------------------------- happy path


def test_fetch_stages_and_verifies_every_member(tmp_path: Path) -> None:
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "stage"

    result = fetch_dockview.fetch(manifest_path, dest, source)

    assert result["cached"] is False
    assert result["version"] == VERSION
    assert (dest / "dockview-core.min.js").read_bytes() == JS_BYTES
    assert (dest / "dockview.css").read_bytes() == CSS_BYTES
    # The unpinned sibling member is NOT staged — only pinned members are extracted.
    assert not (dest / "README.md").exists()


def test_fetch_is_idempotent(tmp_path: Path) -> None:
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "stage"

    first = fetch_dockview.fetch(manifest_path, dest, source)
    second = fetch_dockview.fetch(manifest_path, dest, source)

    assert first["cached"] is False
    assert second["cached"] is True


def test_rerun_restages_when_a_staged_file_is_removed(tmp_path: Path) -> None:
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "stage"

    fetch_dockview.fetch(manifest_path, dest, source)
    (dest / "dockview.css").unlink()
    result = fetch_dockview.fetch(manifest_path, dest, source)

    assert result["cached"] is False
    assert (dest / "dockview.css").read_bytes() == CSS_BYTES


def test_rerun_restages_when_the_pin_changes(tmp_path: Path) -> None:
    """A changed pin must re-stage even though the output files are present (stamp mismatch)."""
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "stage"
    fetch_dockview.fetch(manifest_path, dest, source)

    bumped = dict(manifest, version="9.9.10")
    bumped_source = tmp_path / "source2"
    bumped_source.mkdir()
    tgz = _make_tgz({JS_MEMBER: JS_BYTES, CSS_MEMBER: CSS_BYTES})
    bumped["tarball_file"] = f"{PACKAGE}-9.9.10.tgz"
    bumped["tarball_sha256"] = _sha(tgz)
    (bumped_source / bumped["tarball_file"]).write_bytes(tgz)
    result = fetch_dockview.fetch(_write_manifest(tmp_path, bumped), dest, bumped_source)

    assert result["cached"] is False


# --------------------------------------------------------------------------- fail-closed


def test_tarball_sha_mismatch_is_refused(tmp_path: Path) -> None:
    source, manifest = _make_source(tmp_path)
    manifest["tarball_sha256"] = "0" * 64
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_dockview.VerifyError, match="SHA-256 mismatch"):
        fetch_dockview.fetch(manifest_path, tmp_path / "stage", source)


def test_member_sha_mismatch_is_refused(tmp_path: Path) -> None:
    """The inner-layout guard: the tarball verifies, but a member's own pin does not."""
    source, manifest = _make_source(tmp_path)
    manifest["members"]["dockview.css"]["sha256"] = "1" * 64
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_dockview.VerifyError, match="member 'dockview.css'"):
        fetch_dockview.fetch(manifest_path, tmp_path / "stage", source)


def test_missing_member_is_a_config_error(tmp_path: Path) -> None:
    source, manifest = _make_source(tmp_path, members={JS_MEMBER: JS_BYTES})
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_dockview.FetchError, match="layout changed"):
        fetch_dockview.fetch(manifest_path, tmp_path / "stage", source)


def test_unsafe_member_path_is_refused(tmp_path: Path) -> None:
    source, manifest = _make_source(tmp_path)
    manifest["members"]["dockview.css"]["member"] = "../escape.css"
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_dockview.FetchError, match="unsafe member path"):
        fetch_dockview.fetch(manifest_path, tmp_path / "stage", source)


def test_missing_source_file_is_a_config_error(tmp_path: Path) -> None:
    source, manifest = _make_source(tmp_path)
    manifest["tarball_file"] = "not-there.tgz"
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_dockview.FetchError, match="not found in"):
        fetch_dockview.fetch(manifest_path, tmp_path / "stage", source)


def test_unreadable_manifest_is_a_config_error(tmp_path: Path) -> None:
    with pytest.raises(fetch_dockview.FetchError, match="cannot read manifest"):
        fetch_dockview.fetch(tmp_path / "nope.json", tmp_path / "stage", None)


@pytest.mark.parametrize("drop", ["package", "version", "tarball_sha256", "tarball_file",
                                  "tarball_url_template", "members"])
def test_manifest_missing_required_key_is_refused(tmp_path: Path, drop: str) -> None:
    _, manifest = _make_source(tmp_path)
    manifest.pop(drop)
    with pytest.raises(fetch_dockview.FetchError, match=f"missing '{drop}'"):
        fetch_dockview.check_manifest(manifest)


def test_missing_url_template_is_a_config_error_not_a_refusal(tmp_path: Path, capsys) -> None:
    """A typo'd manifest must exit 2, never 1 — exit 1 means a fail-closed SHA REFUSAL.

    The template is dereferenced unconditionally in ``fetch`` (even on the offline ``--source``
    path, where the URL is unused), so without the ``check_manifest`` guard it raises a bare
    KeyError that escapes ``main``'s handlers as exit 1 — reporting a config typo to CMake as a
    supply-chain tamper.
    """
    source, manifest = _make_source(tmp_path)
    del manifest["tarball_url_template"]
    manifest_path = _write_manifest(tmp_path, manifest)

    rc = fetch_dockview.main(["--manifest", str(manifest_path), "--dest",
                              str(tmp_path / "stage"), "--source", str(source)])

    assert rc == 2
    assert "ERROR" in capsys.readouterr().err


@pytest.mark.parametrize("name", ["../escape.css", "/abs.css", "nested/dockview.css",
                                  "nested\\dockview.css"])
def test_unsafe_member_name_is_refused(tmp_path: Path, name: str) -> None:
    """The member KEY becomes the staged filename, so it needs the same containment check the
    in-tarball member path already gets — otherwise ``<dest>/<name>`` escapes ``--dest``."""
    _, manifest = _make_source(tmp_path)
    manifest["members"][name] = manifest["members"].pop("dockview.css")
    with pytest.raises(fetch_dockview.FetchError, match="unsafe member name"):
        fetch_dockview.check_manifest(manifest)


def test_refused_member_leaves_the_staged_path_untouched(tmp_path: Path) -> None:
    """A REFUSED member must never leave its rejected bytes where the build reads from.

    Regression for the write-then-verify hole: extraction straight to the staged path left the bad
    bytes behind on refusal, and the stamp fast-path re-checks only that the outputs EXIST — so a
    later run whose manifest matched the stamp reported those rejected bytes as ``cached``, turning
    a fail-closed refusal into a silent pass.
    """
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "stage"
    fetch_dockview.fetch(manifest_path, dest, source)

    # A second tarball whose CSS content differs but whose per-member pin was NOT updated: the
    # tarball hash verifies (and differs from the first, so the stamp does not short-circuit),
    # while the member hash does not.
    evil_source, evil = _make_source(
        tmp_path, subdir="evil", members={JS_MEMBER: JS_BYTES, CSS_MEMBER: b".dv-evil{}\n"})

    with pytest.raises(fetch_dockview.VerifyError, match="member 'dockview.css'"):
        fetch_dockview.fetch(_write_manifest(evil_source, evil), dest, evil_source)

    # The refusal left the previously-verified bytes in place and no staging debris behind...
    assert (dest / "dockview.css").read_bytes() == CSS_BYTES
    assert not (dest / "dockview.css.tmp").exists()
    # ...so a later run on the ORIGINAL pin reports `cached` over bytes that really do match it.
    again = fetch_dockview.fetch(manifest_path, dest, source)
    assert again["cached"] is True
    assert (dest / "dockview.css").read_bytes() == CSS_BYTES


def test_member_without_a_sha_pin_is_refused(tmp_path: Path) -> None:
    """A member with no hash would stage an UNVERIFIED file — refuse before downloading."""
    _, manifest = _make_source(tmp_path)
    del manifest["members"]["dockview.css"]["sha256"]
    with pytest.raises(fetch_dockview.FetchError, match="unverified file"):
        fetch_dockview.check_manifest(manifest)


def test_member_without_an_inner_path_is_refused(tmp_path: Path) -> None:
    _, manifest = _make_source(tmp_path)
    del manifest["members"]["dockview.css"]["member"]
    with pytest.raises(fetch_dockview.FetchError, match="in-tarball 'member' path"):
        fetch_dockview.check_manifest(manifest)


# --------------------------------------------------------------------------- CLI surface


def test_main_returns_1_on_verification_failure(tmp_path: Path, capsys) -> None:
    source, manifest = _make_source(tmp_path)
    manifest["tarball_sha256"] = "0" * 64
    manifest_path = _write_manifest(tmp_path, manifest)

    rc = fetch_dockview.main(["--manifest", str(manifest_path), "--dest",
                              str(tmp_path / "stage"), "--source", str(source)])

    assert rc == 1
    assert "REFUSED" in capsys.readouterr().err


def test_main_returns_2_on_config_error(tmp_path: Path, capsys) -> None:
    rc = fetch_dockview.main(["--manifest", str(tmp_path / "nope.json"),
                              "--dest", str(tmp_path / "stage")])

    assert rc == 2
    assert "ERROR" in capsys.readouterr().err


def test_main_returns_0_on_success(tmp_path: Path, capsys) -> None:
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)

    rc = fetch_dockview.main(["--manifest", str(manifest_path), "--dest",
                              str(tmp_path / "stage"), "--source", str(source)])

    assert rc == 0
    assert "staged dockview-core" in capsys.readouterr().out


# ------------------------------------------------ retry/backoff on transient upstream failures
# Mirrors tools/tests/test_fetch_esbuild.py / test_fetch_tsc.py: `_download_with_retry` carries a
# `sleep` parameter that exists ONLY as a test seam, and every other path in this suite drives the
# offline `--source` route — so without these the whole network path (schedule, exhaustion message,
# no-sleep-after-the-last-attempt) ships untested, against R-QA-013.


class _FlakyUrlopen:
    """A urlopen stand-in: raises URLError the first `fail_times` calls, then serves bytes."""

    def __init__(self, fail_times: int, payload: bytes = b"payload-bytes"):
        self.fail_times = fail_times
        self.payload = payload
        self.calls = 0

    def __call__(self, url, timeout=60):  # noqa: ARG002
        self.calls += 1
        if self.calls <= self.fail_times:
            raise fetch_dockview.urllib.error.URLError("simulated transient 504")
        return io.BytesIO(self.payload)


def test_download_retries_then_succeeds(tmp_path: Path, monkeypatch) -> None:
    opener = _FlakyUrlopen(fail_times=2)
    monkeypatch.setattr(fetch_dockview.urllib.request, "urlopen", opener)
    dest = tmp_path / "out.tgz"
    slept: list[float] = []

    fetch_dockview._download_with_retry(
        "https://example.invalid/x", dest, attempts=4, base_delay=0, sleep=slept.append)

    assert dest.read_bytes() == b"payload-bytes"
    assert opener.calls == 3   # two transient failures + one success
    assert len(slept) == 2     # backed off before each retry


def test_download_gives_up_after_attempts(tmp_path: Path, monkeypatch) -> None:
    opener = _FlakyUrlopen(fail_times=99)
    monkeypatch.setattr(fetch_dockview.urllib.request, "urlopen", opener)

    with pytest.raises(fetch_dockview.FetchError, match="after 3 attempts"):
        fetch_dockview._download_with_retry(
            "https://example.invalid/x", tmp_path / "o", attempts=3, base_delay=0,
            sleep=lambda *_: None)

    assert opener.calls == 3


def test_download_backoff_grows_exponentially(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setattr(fetch_dockview.urllib.request, "urlopen", _FlakyUrlopen(fail_times=99))
    slept: list[float] = []

    with pytest.raises(fetch_dockview.FetchError):
        fetch_dockview._download_with_retry(
            "https://example.invalid/x", tmp_path / "o", attempts=4, base_delay=1,
            sleep=slept.append)

    assert slept == [1, 3, 9]  # base * 3**(attempt-1), and NO sleep after the final attempt


# --------------------------------------------------------------------------- the REAL manifest


def test_real_manifest_is_well_formed() -> None:
    manifest = json.loads(
        (fetch_dockview.DEFAULT_MANIFEST).read_text(encoding="utf-8"))
    package, version = fetch_dockview.check_manifest(manifest)
    assert package == "dockview-core"
    assert manifest["license"] == "MIT"
    # Both shipped dist assets are pinned (bundle + stylesheet), each with a 64-hex sha256.
    assert set(manifest["members"]) == {"dockview-core.min.js", "dockview.css"}
    for name, spec in manifest["members"].items():
        assert len(spec["sha256"]) == 64, name
        assert int(spec["sha256"], 16) >= 0, name
    assert len(manifest["tarball_sha256"]) == 64
    assert version


def test_real_manifest_stays_on_the_owner_consented_version() -> None:
    """08 §3: the allowlist approval is VERSION-PINNED to 7.0.2.

    A bump past this version — or adding another ``dockview-*`` package — re-triggers the standing
    owner consent gate. This assertion makes a silent bump fail CI instead of shipping an
    unconsented dependency.
    """
    manifest = json.loads(
        (fetch_dockview.DEFAULT_MANIFEST).read_text(encoding="utf-8"))
    assert manifest["version"] == "7.0.2", (
        "dockview-core is owner-consent-pinned at 7.0.2 (design 08 §3); a bump must re-enter the "
        "consent gate, not ride a routine PR")
