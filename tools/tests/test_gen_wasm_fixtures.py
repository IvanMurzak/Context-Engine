"""Tests for tools/gen_wasm_fixtures.py — the committed sandboxed-WASM fixture generator (R-QA-013).

Pins the two properties that make the committed `.wasm` a trustworthy fixture: (1) the generator is
DETERMINISTIC (byte-identical every run, no ambient state), and (2) the committed artifacts are
REPRODUCIBLE from it (the checked-in `migrate_hp.wasm` / `migrate_hp.wat` are exactly what the
generator emits — so a hand-edit or a stale regen is caught here in the `python-tests` CI job on all
three OSes, no wasmtime needed). Also asserts the module's structural shape (WASM header, the payload
bytes) and the LEB128 encoders, so a future fixture edit can't silently corrupt the binary.
"""

from __future__ import annotations

from pathlib import Path

from conftest import load_tool

gen = load_tool("gen_wasm_fixtures")


# --- LEB128 encoders (the byte-authoritative primitives) -------------------------------------------

def test_uleb128_known_values():
    assert gen.uleb128(0) == b"\x00"
    assert gen.uleb128(8) == b"\x08"
    assert gen.uleb128(127) == b"\x7f"
    assert gen.uleb128(128) == b"\x80\x01"
    assert gen.uleb128(4096) == b"\x80\x20"


def test_sleb128_known_values():
    # Positive values whose 7-bit group has the sign bit set must emit a trailing zero byte.
    assert gen.sleb128(0) == b"\x00"
    assert gen.sleb128(8) == b"\x08"
    assert gen.sleb128(4096) == b"\x80\x20"
    assert gen.sleb128(8192) == b"\x80\xc0\x00"
    assert gen.sleb128(63) == b"\x3f"
    assert gen.sleb128(64) == b"\xc0\x00"  # 0x40 has the sign bit set -> pad positive


def test_uleb128_rejects_negative():
    import pytest

    with pytest.raises(ValueError):
        gen.uleb128(-1)


# --- the encoded module ----------------------------------------------------------------------------

def test_module_has_wasm_header():
    wasm = gen.build_migrate_hp_wasm()
    assert wasm[:4] == b"\x00asm", "WASM magic"
    assert wasm[4:8] == b"\x01\x00\x00\x00", "WASM version 1"


def test_module_is_deterministic():
    assert gen.build_migrate_hp_wasm() == gen.build_migrate_hp_wasm()


def test_module_embeds_the_output_payload():
    wasm = gen.build_migrate_hp_wasm()
    assert gen.MIGRATE_HP_OUTPUT == b'{"hp":2}'
    assert gen.MIGRATE_HP_OUTPUT in wasm, "the data segment carries the canonical output payload"


def test_module_declares_the_frozen_abi_export_names():
    wasm = gen.build_migrate_hp_wasm()
    # Zero-import guest ABI: the export names live verbatim in the export section.
    assert b"memory" in wasm
    assert b"ctx_alloc" in wasm
    assert b"ctx_migrate" in wasm


# --- committed-fixture reproducibility (the CI gate) -----------------------------------------------

def test_committed_wasm_matches_generator():
    committed = (gen.FIXTURE_DIR / "migrate_hp.wasm").read_bytes()
    assert committed == gen.build_migrate_hp_wasm(), (
        "src/runtime/wasm/fixtures/migrate_hp.wasm is stale — "
        "run `python3 tools/gen_wasm_fixtures.py`"
    )


def test_committed_wat_matches_source():
    committed = (gen.FIXTURE_DIR / "migrate_hp.wat").read_bytes()
    assert committed == gen._wat_bytes(gen.MIGRATE_HP_WAT), (
        "src/runtime/wasm/fixtures/migrate_hp.wat is stale — "
        "run `python3 tools/gen_wasm_fixtures.py`"
    )


def test_check_fixtures_reports_no_drift():
    assert gen.check_fixtures() == []


def test_write_then_check_roundtrips(tmp_path: Path):
    written = gen.write_fixtures(tmp_path)
    assert {p.name for p in written} == {"migrate_hp.wasm", "migrate_hp.wat"}
    assert gen.check_fixtures(tmp_path) == []
    # The freshly-written bytes equal the committed bytes (same generator, no host dependence).
    assert (tmp_path / "migrate_hp.wasm").read_bytes() == (
        gen.FIXTURE_DIR / "migrate_hp.wasm"
    ).read_bytes()


def test_check_detects_drift(tmp_path: Path):
    gen.write_fixtures(tmp_path)
    (tmp_path / "migrate_hp.wasm").write_bytes(b"\x00asm\x01\x00\x00\x00corrupt")
    drift = gen.check_fixtures(tmp_path)
    assert any("migrate_hp.wasm" in d for d in drift)


def test_main_check_passes_on_committed_fixtures():
    assert gen.main(["--check"]) == 0
