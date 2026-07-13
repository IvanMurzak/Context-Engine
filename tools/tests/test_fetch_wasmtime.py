"""Tests for tools/fetch_wasmtime.py — the pinned wasmtime C-API fetch/verify gate (R-QA-013).

The direct sibling of tools/tests/test_fetch_v8.py. Covers the happy path (offline ``--source``,
exercising the real verify + archive extraction + include/lib staging), every fail-closed path
(SHA mismatch, unknown platform, version-coherence skew, unknown archive format, missing source
file), idempotency, and the transient-download retry/backoff — all on tiny synthetic archives so
no network is touched. A final integration block asserts the REAL tools/wasmtime-prebuilt.json is
well-formed and version-coherent, keeping the pin manifest itself honest.
"""

from __future__ import annotations

import hashlib
import io
import json
import tarfile
import zipfile
from pathlib import Path

import pytest
from conftest import load_tool

fetch_wasmtime = load_tool("fetch_wasmtime")

VERSION = "9.9.9"
PLATFORM = "x86_64-linux"
LIB_NAME = "libwasmtime.a"
LIB_BYTES = b"fake-static-archive-contents\x00\x01\x02"
HEADER_FILES = {
    "include/wasmtime.h": b"// fake wasmtime.h\n",
    "include/wasm.h": b"// fake wasm.h\n",
    "include/wasmtime/config.h": b"// fake wasmtime/config.h\n",
}


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _archive_members(root: str) -> dict[str, bytes]:
    """The C-API archive's internal <root>/include/* + <root>/lib/<lib> layout."""
    members = {f"{root}/{rel}": data for rel, data in HEADER_FILES.items()}
    members[f"{root}/lib/{LIB_NAME}"] = LIB_BYTES
    return members


def _make_source(tmp_path: Path, fmt: str = "tar.gz") -> tuple[Path, dict]:
    """Build a synthetic --source dir (one C-API archive) and a matching manifest.

    fmt is tar.gz (default — always available, no lzma dependency) or zip, so both archive
    code paths in fetch_wasmtime._extract_archive are exercised by the suite.
    """
    source = tmp_path / "source"
    source.mkdir()

    root = f"wasmtime-v{VERSION}-{PLATFORM}-c-api"
    archive_name = f"{root}.{fmt}"
    members = _archive_members(root)

    if fmt == "zip":
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
            for name, data in members.items():
                zf.writestr(name, data)
        archive_bytes = buf.getvalue()
    else:
        mode = "w:xz" if fmt == "tar.xz" else "w:gz"
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode=mode) as tar:
            for name, data in members.items():
                info = tarfile.TarInfo(name=name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        archive_bytes = buf.getvalue()

    (source / archive_name).write_bytes(archive_bytes)

    manifest = {
        "wasmtime_version": VERSION,
        "release_url_template": "https://example.invalid/v{wasmtime_version}/{archive}",
        "header": "wasmtime.h",
        "platforms": {
            PLATFORM: {
                "archive": archive_name,
                "format": fmt,
                "sha256": _sha(archive_bytes),
                "lib": LIB_NAME,
            },
        },
    }
    return source, manifest


def _write_manifest(tmp_path: Path, manifest: dict) -> Path:
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


@pytest.mark.parametrize("fmt", ["tar.gz", "zip"])
def test_offline_happy_path_stages_lib_and_headers(tmp_path: Path, fmt: str):
    source, manifest = _make_source(tmp_path, fmt=fmt)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    result = fetch_wasmtime.fetch(manifest_path, PLATFORM, dest, source=source)

    assert result["cached"] is False
    lib = Path(result["lib"])
    assert lib.name == LIB_NAME
    assert lib.read_bytes() == LIB_BYTES  # extracted byte-exact
    include = Path(result["include"])
    for rel, data in HEADER_FILES.items():
        # <root>/include/* is flattened to <dest>/include/* (root + include/ prefix stripped).
        staged = include / rel[len("include/"):]
        assert staged.read_bytes() == data  # prefix stripped, nested dirs kept


def test_second_run_is_cached(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    fetch_wasmtime.fetch(manifest_path, PLATFORM, dest, source=source)
    second = fetch_wasmtime.fetch(manifest_path, PLATFORM, dest, source=source)
    assert second["cached"] is True


def test_sha_mismatch_is_refused_fail_closed(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["platforms"][PLATFORM]["sha256"] = "0" * 64  # tampered pin
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_wasmtime.VerifyError):
        fetch_wasmtime.fetch(manifest_path, PLATFORM, tmp_path / "out", source=source)


def test_sha_mismatch_exit_code_is_one(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["platforms"][PLATFORM]["sha256"] = "0" * 64
    manifest_path = _write_manifest(tmp_path, manifest)
    rc = fetch_wasmtime.main([
        "--manifest", str(manifest_path), "--platform", PLATFORM,
        "--dest", str(tmp_path / "out"), "--source", str(source),
    ])
    assert rc == 1  # fail-closed refusal


def test_unknown_platform_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_wasmtime.FetchError):
        fetch_wasmtime.fetch(manifest_path, "sparc-unknown-solaris", tmp_path / "out", source=source)


def test_version_coherence_skew_is_rejected(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    # archive name embeds a version != wasmtime_version -> refused before any download.
    manifest["platforms"][PLATFORM]["archive"] = "wasmtime-v1.2.3-x86_64-linux-c-api.tar.gz"
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_wasmtime.FetchError, match="version-coherence"):
        fetch_wasmtime.fetch(manifest_path, PLATFORM, tmp_path / "out", source=source)


def test_unknown_archive_format_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["platforms"][PLATFORM]["format"] = "rar"
    manifest["platforms"][PLATFORM]["archive"] = f"wasmtime-v{VERSION}-{PLATFORM}-c-api.rar"
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_wasmtime.FetchError, match="format"):
        fetch_wasmtime.fetch(manifest_path, PLATFORM, tmp_path / "out", source=source)


def test_missing_source_file_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    (source / manifest["platforms"][PLATFORM]["archive"]).unlink()  # archive absent from --source
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_wasmtime.FetchError):
        fetch_wasmtime.fetch(manifest_path, PLATFORM, tmp_path / "out", source=source)


def test_layout_change_missing_lib_is_config_error(tmp_path: Path):
    """An archive that verifies but lacks the pinned lib is a fail-closed config error."""
    source, manifest = _make_source(tmp_path)
    manifest["platforms"][PLATFORM]["lib"] = "libwasmtime_absent.a"  # not in the archive
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_wasmtime.FetchError, match="missing lib"):
        fetch_wasmtime.fetch(manifest_path, PLATFORM, tmp_path / "out", source=source)


# --- integration: the REAL pin manifest must stay well-formed + coherent -------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
REAL_MANIFEST = REPO_ROOT / "tools" / "wasmtime-prebuilt.json"
REQUIRED_PLATFORMS = {
    "x86_64-linux",
    "aarch64-macos",
    "x86_64-windows",
}


def test_real_manifest_is_coherent_and_complete():
    manifest = json.loads(REAL_MANIFEST.read_text(encoding="utf-8"))
    version = fetch_wasmtime.check_coherence(manifest)  # raises on skew / bad format
    assert version == manifest["wasmtime_version"]
    # All three CI matrix platforms (Linux-x64 / macOS-ARM64 / Win-x64) present + pinned.
    assert REQUIRED_PLATFORMS.issubset(manifest["platforms"].keys())
    for plat, entry in manifest["platforms"].items():
        assert len(entry["sha256"]) == 64, plat
        assert all(c in "0123456789abcdefABCDEF" for c in entry["sha256"]), plat  # hex
        assert entry["archive"] and entry["lib"], plat
        assert entry["format"] in fetch_wasmtime._VALID_FORMATS, plat
    assert manifest["header"]
    # The URL template must accept the documented substitution keys without KeyError.
    formatted = manifest["release_url_template"].format(
        wasmtime_version=version, archive=manifest["platforms"]["x86_64-linux"]["archive"])
    assert formatted.startswith("https://")


# --- retry/backoff for transient upstream failures (Context-Engine#129) ----------------------
# Identical harness to test_fetch_v8.py — the resilience code is copied verbatim from fetch_v8.py.


class _FlakyUrlopen:
    """A urlopen stand-in: raises URLError the first `fail_times` calls, then serves bytes."""

    def __init__(self, fail_times: int, payload: bytes = b"payload-bytes"):
        self.fail_times = fail_times
        self.payload = payload
        self.calls = 0

    def __call__(self, url, timeout=60):  # noqa: ARG002
        self.calls += 1
        if self.calls <= self.fail_times:
            raise fetch_wasmtime.urllib.error.URLError("simulated transient 504")
        return io.BytesIO(self.payload)


def test_download_retries_then_succeeds(tmp_path: Path, monkeypatch):
    opener = _FlakyUrlopen(fail_times=2)
    monkeypatch.setattr(fetch_wasmtime.urllib.request, "urlopen", opener)
    dest = tmp_path / "out.bin"
    slept: list[float] = []
    fetch_wasmtime._download_with_retry(
        "https://example.invalid/x", dest, attempts=4, base_delay=0, sleep=slept.append)
    assert dest.read_bytes() == b"payload-bytes"
    assert opener.calls == 3   # two transient failures + one success
    assert len(slept) == 2     # backed off before each retry


def test_download_gives_up_after_attempts(tmp_path: Path, monkeypatch):
    opener = _FlakyUrlopen(fail_times=99)
    monkeypatch.setattr(fetch_wasmtime.urllib.request, "urlopen", opener)
    with pytest.raises(fetch_wasmtime.FetchError, match="after 3 attempts"):
        fetch_wasmtime._download_with_retry(
            "https://example.invalid/x", tmp_path / "o", attempts=3, base_delay=0,
            sleep=lambda *_: None)
    assert opener.calls == 3


def test_download_backoff_grows_exponentially(tmp_path: Path, monkeypatch):
    monkeypatch.setattr(fetch_wasmtime.urllib.request, "urlopen", _FlakyUrlopen(fail_times=99))
    slept: list[float] = []
    with pytest.raises(fetch_wasmtime.FetchError):
        fetch_wasmtime._download_with_retry(
            "https://example.invalid/x", tmp_path / "o", attempts=4, base_delay=1,
            sleep=slept.append)
    assert slept == [1, 3, 9]  # exponential: base * 3**(attempt-1), no sleep after the last try
