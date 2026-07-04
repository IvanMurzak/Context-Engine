"""Tests for tools/gen_nfc_tables.py — the NFC-table generator behind src/editor/serializer.

R-QA-013: the generator is Python tooling, so its behavior ships with pytest coverage. The
normative consumer-side proof (the C++ NFC implementation over the generated tables) lives in the
serializer's own ctest suites + the committed test-vector corpus; THESE tests pin the generator's
contract: determinism, known-entry correctness, exclusion filtering, and the failure path.
"""

import sys
import unicodedata
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import gen_nfc_tables  # noqa: E402


def test_emit_is_deterministic(tmp_path):
    a = tmp_path / "a.inc"
    b = tmp_path / "b.inc"
    gen_nfc_tables.emit(str(a))
    gen_nfc_tables.emit(str(b))
    assert a.read_bytes() == b.read_bytes()


def test_emitted_tables_contain_known_entries(tmp_path):
    out = tmp_path / "tables.inc"
    gen_nfc_tables.emit(str(out))
    text = out.read_text(encoding="utf-8")

    # Records its Unicode source version.
    assert unicodedata.unidata_version in text

    # Combining acute (U+0301, ccc 230) is a CCC entry.
    assert "{0x301, 230}" in text

    # e + combining acute -> é is a primary composite: key (0x65 << 21) | 0x301.
    key = (0x65 << 21) | 0x301
    assert f"{{0x{key:X}ULL, 0xE9}}" in text

    # ANGSTROM SIGN (U+212B) has a singleton decomposition — present in the decomp table.
    assert "{0x212B, " in text


def test_composition_exclusions_are_filtered():
    comps = gen_nfc_tables.collect_compositions()
    pairs = {(f, s) for f, s, _ in comps}
    # U+0958 DEVANAGARI QA decomposes to 0915+093C but is composition-EXCLUDED: the pair must
    # NOT recompose. U+0929 NNNA (0928+093C) is NOT excluded: it must be present.
    assert (0x0915, 0x093C) not in pairs
    assert (0x0928, 0x093C) in pairs
    # Hangul is algorithmic — never table-driven.
    assert all(not (0xAC00 <= composed < 0xAC00 + 11172) for _, _, composed in comps)


def test_decompositions_are_full_nfd():
    entries, data = gen_nfc_tables.collect_decompositions()
    by_cp = {cp: data[off : off + ln] for cp, off, ln in entries}
    # U+1E09 (c with cedilla and acute) fully decomposes to THREE codepoints (recursive NFD).
    assert by_cp[0x1E09] == [0x63, 0x0327, 0x0301]
    # ASCII never decomposes.
    assert all(cp >= 0x80 for cp in by_cp)


def test_qc_ranges_flag_maybe_and_no():
    comps = gen_nfc_tables.collect_compositions()
    ranges = gen_nfc_tables.collect_qc_ranges(comps)

    def flagged(cp):
        return any(lo <= cp <= hi for lo, hi in ranges)

    assert flagged(0x0301)  # combining acute: Maybe (second of pairs)
    assert flagged(0x212B)  # Angstrom: No (not NFC by itself)
    assert flagged(0x1161)  # Hangul V jamo: Maybe (algorithmic second)
    assert not flagged(0x0041)  # plain ASCII letter
    assert not flagged(0x00E9)  # composed é is quick-check YES


def test_main_fails_on_unwritable_output(tmp_path):
    missing_dir = tmp_path / "does-not-exist" / "tables.inc"
    with pytest.raises(OSError):
        gen_nfc_tables.emit(str(missing_dir))
