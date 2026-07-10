"""Tests for tools/fetch_tsc.py — the pinned tsgo (native TypeScript) prebuilt fetch/verify gate
(R-QA-013, issue #85).

Covers the happy path (offline ``--source``, exercising the real verify + WHOLE-lib-dir member
extraction + the executable bit + the adjacent lib.*.d.ts files), every fail-closed path (SHA
mismatch, unknown platform, version-coherence skew, missing source file, missing binary member),
and idempotency — all on tiny synthetic npm-style tarballs so no network is touched. A final
integration block asserts the REAL tools/tsc-toolchain.json is well-formed, version-coherent, and
covers the three CI platforms, keeping the pin manifest itself honest.

The ONE structural difference from test_fetch_esbuild.py: tsgo ships a member DIRECTORY
(``package/lib/`` = the binary next to its lib.*.d.ts type library), not a single binary member —
so the synthetic tarball carries several files under member_dir and the test asserts they ALL land.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import stat
import tarfile
from pathlib import Path

import pytest
from conftest import load_tool

fetch_tsc = load_tool("fetch_tsc")

VERSION = "7.0.0-dev.99999999.9"
BINARY_BYTES = b"\x7ffake-tsgo-native-binary\x00\x01\x02"
LIBDTS_BYTES = b"// fake lib.d.ts\ndeclare var globalThis: any;\n"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _make_package_tgz(member_dir: str, binary: str) -> bytes:
    """Build a synthetic @typescript/native-preview-<platform> npm tarball carrying the binary
    ALONGSIDE a lib.d.ts under member_dir, plus a non-member sibling (proving the dir filter)."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        # The binary member (executable).
        binfo = tarfile.TarInfo(name=f"{member_dir}/{binary}")
        binfo.size = len(BINARY_BYTES)
        binfo.mode = 0o755
        tar.addfile(binfo, io.BytesIO(BINARY_BYTES))
        # A default-lib type file that MUST be staged next to the binary (tsgo panics without it).
        linfo = tarfile.TarInfo(name=f"{member_dir}/lib.d.ts")
        linfo.size = len(LIBDTS_BYTES)
        tar.addfile(linfo, io.BytesIO(LIBDTS_BYTES))
        # A non-member sibling proves extraction is scoped to member_dir only.
        readme = b"# tsgo\n"
        rinfo = tarfile.TarInfo(name="package/README.md")
        rinfo.size = len(readme)
        tar.addfile(rinfo, io.BytesIO(readme))
    return buf.getvalue()


def _make_source(tmp_path: Path, platform: str, binary: str) -> tuple[Path, dict]:
    """Build a synthetic --source dir (one npm tarball) and a matching manifest."""
    source = tmp_path / "source"
    source.mkdir()
    member_dir = "package/lib"
    tgz = _make_package_tgz(member_dir, binary)
    file_name = f"native-preview-{platform}-{VERSION}.tgz"
    (source / file_name).write_bytes(tgz)

    manifest = {
        "tsc_version": VERSION,
        "package_url_template":
            "https://example.invalid/@typescript/native-preview-{platform}/{tsc_version}",
        "platforms": {
            platform: {
                "package": f"@typescript/native-preview-{platform}",
                "file": file_name,
                "sha256": _sha(tgz),
                "member_dir": member_dir,
                "binary": binary,
            },
        },
    }
    return source, manifest


def _write_manifest(tmp_path: Path, manifest: dict) -> Path:
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def test_offline_happy_path_stages_binary_and_libs(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    result = fetch_tsc.fetch(manifest_path, "linux-x64", dest, source=source)

    assert result["cached"] is False
    binary = Path(result["binary"])
    assert binary.name == "tsgo"
    assert binary.read_bytes() == BINARY_BYTES  # exactly the pinned binary, byte-exact
    # The adjacent lib.d.ts MUST be staged next to the binary (tsgo resolves libs from its own dir).
    assert (binary.parent / "lib.d.ts").read_bytes() == LIBDTS_BYTES
    # The non-member sibling (package/README.md) is NOT staged — extraction is scoped to member_dir.
    assert not (dest / "README.md").exists()
    if os.name != "nt":
        assert binary.stat().st_mode & stat.S_IXUSR  # made executable on POSIX


def test_win32_binary_name(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "win32-x64", "tsgo.exe")
    manifest_path = _write_manifest(tmp_path, manifest)
    result = fetch_tsc.fetch(manifest_path, "win32-x64", tmp_path / "out", source=source)
    assert Path(result["binary"]).name == "tsgo.exe"
    assert Path(result["binary"]).read_bytes() == BINARY_BYTES


def test_second_run_is_cached(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    fetch_tsc.fetch(manifest_path, "linux-x64", dest, source=source)
    second = fetch_tsc.fetch(manifest_path, "linux-x64", dest, source=source)
    assert second["cached"] is True


def test_sha_mismatch_is_refused_fail_closed(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest["platforms"]["linux-x64"]["sha256"] = "0" * 64  # tampered pin
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_tsc.VerifyError):
        fetch_tsc.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_sha_mismatch_exit_code_is_one(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest["platforms"]["linux-x64"]["sha256"] = "0" * 64
    manifest_path = _write_manifest(tmp_path, manifest)
    rc = fetch_tsc.main([
        "--manifest", str(manifest_path), "--platform", "linux-x64",
        "--dest", str(tmp_path / "out"), "--source", str(source),
    ])
    assert rc == 1  # fail-closed refusal


def test_unknown_platform_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_tsc.FetchError):
        fetch_tsc.fetch(manifest_path, "sparc-solaris", tmp_path / "out", source=source)


def test_version_coherence_skew_is_rejected(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest["platforms"]["linux-x64"]["file"] = "native-preview-linux-x64-1.2.3.tgz"  # ver skew
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_tsc.FetchError, match="version-coherence"):
        fetch_tsc.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_package_key_skew_is_rejected(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest["platforms"]["linux-x64"]["package"] = "@typescript/native-preview-darwin-arm64"
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_tsc.FetchError, match="version-coherence"):
        fetch_tsc.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_missing_source_file_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    (source / manifest["platforms"]["linux-x64"]["file"]).unlink()  # tarball absent from --source
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_tsc.FetchError):
        fetch_tsc.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_missing_binary_member_is_config_error(tmp_path: Path):
    # Manifest names a binary the member_dir does not carry — a layout-change tripwire.
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest["platforms"]["linux-x64"]["binary"] = "tsgo-nonexistent"
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_tsc.FetchError, match="binary"):
        fetch_tsc.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_empty_member_dir_is_config_error(tmp_path: Path):
    # member_dir names a prefix the tarball has no files under — a layout-change tripwire.
    source, manifest = _make_source(tmp_path, "linux-x64", "tsgo")
    manifest["platforms"]["linux-x64"]["member_dir"] = "package/nonexistent"
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_tsc.FetchError, match="member_dir"):
        fetch_tsc.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


# --- integration: the REAL pin manifest must stay well-formed + coherent -------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
REAL_MANIFEST = REPO_ROOT / "tools" / "tsc-toolchain.json"
REQUIRED_PLATFORMS = {"linux-x64", "darwin-arm64", "win32-x64"}


def test_real_manifest_is_coherent_and_complete():
    manifest = json.loads(REAL_MANIFEST.read_text(encoding="utf-8"))
    version = fetch_tsc.check_coherence(manifest)  # raises on skew
    assert version == manifest["tsc_version"]
    # All three CI matrix platforms (Linux-x64 / macOS-ARM64 / Win-x64) present + pinned.
    assert REQUIRED_PLATFORMS.issubset(manifest["platforms"].keys())
    for name, spec in manifest["platforms"].items():
        assert len(spec["sha256"]) == 64, name
        assert all(c in "0123456789abcdefABCDEF" for c in spec["sha256"]), name  # hex
        assert spec["file"] and spec["member_dir"] and spec["binary"], name
    # win32 ships tsgo.exe; the unix platforms ship a bare `tsgo`.
    assert manifest["platforms"]["win32-x64"]["binary"] == "tsgo.exe"
    assert manifest["platforms"]["linux-x64"]["binary"] == "tsgo"
    # The URL template must accept the documented substitution keys without KeyError.
    formatted = manifest["package_url_template"].format(
        platform="linux-x64", tsc_version=version)
    assert formatted.startswith("https://")


# --- retry/backoff for transient upstream failures (mirrors fetch_esbuild) --------------------


class _FlakyUrlopen:
    """A urlopen stand-in: raises URLError the first `fail_times` calls, then serves bytes."""

    def __init__(self, fail_times: int, payload: bytes = b"payload-bytes"):
        self.fail_times = fail_times
        self.payload = payload
        self.calls = 0

    def __call__(self, url, timeout=60):  # noqa: ARG002
        self.calls += 1
        if self.calls <= self.fail_times:
            raise fetch_tsc.urllib.error.URLError("simulated transient 504")
        return io.BytesIO(self.payload)


def test_download_retries_then_succeeds(tmp_path: Path, monkeypatch):
    opener = _FlakyUrlopen(fail_times=2)
    monkeypatch.setattr(fetch_tsc.urllib.request, "urlopen", opener)
    dest = tmp_path / "out.bin"
    slept: list[float] = []
    fetch_tsc._download_with_retry(
        "https://example.invalid/x", dest, attempts=4, base_delay=0, sleep=slept.append)
    assert dest.read_bytes() == b"payload-bytes"
    assert opener.calls == 3   # two transient failures + one success
    assert len(slept) == 2     # backed off before each retry


def test_download_gives_up_after_attempts(tmp_path: Path, monkeypatch):
    opener = _FlakyUrlopen(fail_times=99)
    monkeypatch.setattr(fetch_tsc.urllib.request, "urlopen", opener)
    with pytest.raises(fetch_tsc.FetchError, match="after 3 attempts"):
        fetch_tsc._download_with_retry(
            "https://example.invalid/x", tmp_path / "o", attempts=3, base_delay=0,
            sleep=lambda *_: None)
    assert opener.calls == 3
