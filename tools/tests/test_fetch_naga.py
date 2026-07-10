"""Tests for tools/fetch_naga.py — the pinned naga-cli install/verify gate (R-QA-013).

Covers manifest validation (missing keys, unreadable file), the fail-closed version
verification, the idempotent no-op path (a staged binary reporting the pin needs no
cargo), and the cargo-missing configuration error — all with the subprocess seam
monkeypatched so no network / Rust toolchain is touched. A final integration block
asserts the REAL tools/naga-toolchain.json pin manifest is well-formed, keeping the pin
itself honest (the same pattern as test_fetch_esbuild.py's manifest block).
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from conftest import load_tool

fetch_naga = load_tool("fetch_naga")

PIN = {"crate": "naga-cli", "version": "9.9.9", "binary": "naga"}


def _write_manifest(tmp_path: Path, data: dict) -> Path:
    path = tmp_path / "naga-toolchain.json"
    path.write_text(json.dumps(data), encoding="utf-8")
    return path


# --- manifest validation -------------------------------------------------------------


def test_load_manifest_happy(tmp_path: Path) -> None:
    manifest = fetch_naga.load_manifest(_write_manifest(tmp_path, PIN))
    assert manifest["crate"] == "naga-cli"
    assert manifest["version"] == "9.9.9"


@pytest.mark.parametrize("missing", ["crate", "version", "binary"])
def test_load_manifest_missing_key_fails(tmp_path: Path, missing: str) -> None:
    data = {k: v for k, v in PIN.items() if k != missing}
    with pytest.raises(fetch_naga.FetchError, match=missing):
        fetch_naga.load_manifest(_write_manifest(tmp_path, data))


def test_load_manifest_bad_json_fails(tmp_path: Path) -> None:
    path = tmp_path / "broken.json"
    path.write_text("{not json", encoding="utf-8")
    with pytest.raises(fetch_naga.FetchError):
        fetch_naga.load_manifest(path)


# --- fail-closed version verification -------------------------------------------------


def test_verify_accepts_exact_pin(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(fetch_naga, "reported_version", lambda _b: "9.9.9")
    fetch_naga.verify(Path("naga"), "9.9.9")  # must not raise


@pytest.mark.parametrize("reported", ["9.9.8", "naga 9.9.9", "", None])
def test_verify_rejects_mismatch(monkeypatch: pytest.MonkeyPatch, reported) -> None:
    monkeypatch.setattr(fetch_naga, "reported_version", lambda _b: reported)
    with pytest.raises(fetch_naga.VerifyError):
        fetch_naga.verify(Path("naga"), "9.9.9")


# --- main() paths ---------------------------------------------------------------------


def test_main_noop_when_staged_binary_matches(tmp_path: Path,
                                              monkeypatch: pytest.MonkeyPatch) -> None:
    """A staged binary reporting the pin exits 0 WITHOUT cargo (the CI cache-hit path)."""
    manifest_path = _write_manifest(tmp_path, PIN)
    binary = fetch_naga.binary_path(tmp_path, PIN)
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"\x7ffake-naga")
    monkeypatch.setattr(fetch_naga, "reported_version", lambda _b: "9.9.9")

    def boom(*_a, **_k):  # cargo must never be consulted on the no-op path
        raise AssertionError("install() must not run on the no-op path")

    monkeypatch.setattr(fetch_naga, "install", boom)
    monkeypatch.setattr(fetch_naga.sys, "argv",
                        ["fetch_naga", "--manifest", str(manifest_path),
                         "--dest", str(tmp_path)])
    assert fetch_naga.main() == 0


def test_main_verify_failure_exits_1(tmp_path: Path,
                                     monkeypatch: pytest.MonkeyPatch) -> None:
    manifest_path = _write_manifest(tmp_path, PIN)
    monkeypatch.setattr(fetch_naga, "reported_version", lambda _b: "0.0.1")
    monkeypatch.setattr(fetch_naga, "install", lambda *_a, **_k: None)
    monkeypatch.setattr(fetch_naga.sys, "argv",
                        ["fetch_naga", "--manifest", str(manifest_path),
                         "--dest", str(tmp_path)])
    assert fetch_naga.main() == 1


def test_main_cargo_missing_exits_2(tmp_path: Path,
                                    monkeypatch: pytest.MonkeyPatch) -> None:
    manifest_path = _write_manifest(tmp_path, PIN)
    monkeypatch.setattr(fetch_naga.shutil, "which", lambda _c: None)
    monkeypatch.setattr(fetch_naga.sys, "argv",
                        ["fetch_naga", "--manifest", str(manifest_path),
                         "--dest", str(tmp_path)])
    assert fetch_naga.main() == 2


# --- the real pin manifest stays honest ------------------------------------------------


def test_real_manifest_is_well_formed() -> None:
    manifest = fetch_naga.load_manifest(fetch_naga.DEFAULT_MANIFEST)
    assert manifest["crate"] == "naga-cli"
    assert manifest["binary"] == "naga"
    parts = manifest["version"].split(".")
    assert len(parts) == 3 and all(p.isdigit() for p in parts), \
        "pin must be an exact x.y.z version (no ranges — determinism depends on it)"
