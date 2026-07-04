#!/usr/bin/env python3
"""Generate the Unicode NFC normalization tables for the canonical-JSON serializer.

R-FILE-001 mandates NFC Unicode normalization for every string in the canonical form, with
an ASCII quick-check fast path. The C++ implementation (src/editor/serializer/src/nfc.cpp)
is table-driven; this script derives those tables from the Python interpreter's `unicodedata`
module and emits them as a committed C++ include file. Regeneration is manual and rare:

    python tools/gen_nfc_tables.py            # rewrites src/editor/serializer/src/nfc_tables.inc
    python tools/gen_nfc_tables.py --out <p>  # writes elsewhere (tests use a temp path)

Determinism: for one interpreter (one Unicode version) the output is byte-identical across
runs — codepoints are walked in ascending order and formatting is fixed. The Unicode
normalization stability policy freezes canonical decompositions and combining classes for
every assigned character, so regenerating under a NEWER Unicode version only APPENDS
entries for newly assigned characters; existing canonical forms never change. The emitted
header records the source Unicode version.

Four tables are emitted (all sorted by codepoint / key for binary search):

  - kCccEntries      — (codepoint, canonical combining class) for every cp with ccc != 0.
  - kDecompEntries   — (codepoint, offset, length) into kDecompData: the FULL canonical
                       (NFD) decomposition of every cp whose NFD form differs from itself.
                       The Hangul syllable block is EXCLUDED (algorithmic, UAX #15 §3.12).
  - kCompEntries     — ((first << 21) | second, composed) for every primary composite:
                       canonical two-codepoint decompositions that NFC actually recomposes
                       (composition exclusions and non-starter decompositions are already
                       filtered out by construction). Hangul is excluded (algorithmic).
  - kQcNoOrMaybe     — merged, inclusive codepoint ranges for which NFC quick-check is not
                       definitively YES: characters that are not NFC by themselves (No) plus
                       characters that may combine with a preceding character (Maybe). The
                       C++ quick check treats both conservatively: their presence routes the
                       string through full normalization (a no-op when already NFC).
"""

from __future__ import annotations

import argparse
import sys
import unicodedata

MAX_CP = 0x110000
HANGUL_S_BASE = 0xAC00
HANGUL_S_COUNT = 11172
HANGUL_V_FIRST, HANGUL_V_LAST = 0x1161, 0x1175  # jungseong (medial vowels)
HANGUL_T_FIRST, HANGUL_T_LAST = 0x11A8, 0x11C2  # jongseong (trailing consonants)


def _is_surrogate(cp: int) -> bool:
    return 0xD800 <= cp <= 0xDFFF


def _is_hangul_syllable(cp: int) -> bool:
    return HANGUL_S_BASE <= cp < HANGUL_S_BASE + HANGUL_S_COUNT


def collect_ccc() -> list[tuple[int, int]]:
    out = []
    for cp in range(MAX_CP):
        if _is_surrogate(cp):
            continue
        ccc = unicodedata.combining(chr(cp))
        if ccc:
            out.append((cp, ccc))
    return out


def collect_decompositions() -> tuple[list[tuple[int, int, int]], list[int]]:
    """Full canonical (NFD) decompositions, Hangul excluded; (cp, offset, len) + flat data."""
    entries: list[tuple[int, int, int]] = []
    data: list[int] = []
    for cp in range(MAX_CP):
        if _is_surrogate(cp) or _is_hangul_syllable(cp):
            continue
        nfd = unicodedata.normalize("NFD", chr(cp))
        cps = [ord(c) for c in nfd]
        if cps == [cp]:
            continue
        entries.append((cp, len(data), len(cps)))
        data.extend(cps)
    return entries, data


def collect_compositions() -> list[tuple[int, int, int]]:
    """Primary composites as (first, second, composed), exclusions filtered by construction."""
    out = []
    for cp in range(MAX_CP):
        if _is_surrogate(cp) or _is_hangul_syllable(cp):
            continue
        raw = unicodedata.decomposition(chr(cp))
        if not raw or raw.startswith("<"):  # no decomposition, or a compatibility one
            continue
        parts = [int(x, 16) for x in raw.split()]
        if len(parts) != 2:
            continue
        first, second = parts
        # A pair is a primary composite exactly when NFC recomposes it to cp: this filters
        # the composition-exclusion list AND non-starter decompositions in one check.
        if unicodedata.normalize("NFC", chr(first) + chr(second)) == chr(cp):
            out.append((first, second, cp))
    return out


def collect_qc_ranges(compositions: list[tuple[int, int, int]]) -> list[tuple[int, int]]:
    """Merged ranges of cps whose NFC quick-check is No or Maybe (conservative union)."""
    flagged = set()
    for cp in range(MAX_CP):
        if _is_surrogate(cp):
            continue
        # No: a character that is not NFC by itself (singleton/non-starter decompositions,
        # composition-excluded precomposites, Hangul handled below via is_normalized too).
        if not unicodedata.is_normalized("NFC", chr(cp)):
            flagged.add(cp)
    # Maybe: characters that can combine with a PRECEDING character.
    for _, second, _ in compositions:
        flagged.add(second)
    flagged.update(range(HANGUL_V_FIRST, HANGUL_V_LAST + 1))  # Hangul V (algorithmic LV)
    flagged.update(range(HANGUL_T_FIRST, HANGUL_T_LAST + 1))  # Hangul T (algorithmic LVT)

    ranges: list[tuple[int, int]] = []
    for cp in sorted(flagged):
        if ranges and cp == ranges[-1][1] + 1:
            ranges[-1] = (ranges[-1][0], cp)
        else:
            ranges.append((cp, cp))
    return ranges


def emit(out_path: str) -> int:
    ccc = collect_ccc()
    decomp_entries, decomp_data = collect_decompositions()
    compositions = collect_compositions()
    qc_ranges = collect_qc_ranges(compositions)

    lines: list[str] = []
    push = lines.append
    push("// nfc_tables.inc — Unicode NFC normalization tables (R-FILE-001).")
    push("//")
    push("// GENERATED by tools/gen_nfc_tables.py — DO NOT EDIT BY HAND. Regenerate with:")
    push("//     python tools/gen_nfc_tables.py")
    push(f"// Source: Python unicodedata, Unicode {unicodedata.unidata_version}. The Unicode")
    push("// normalization stability policy freezes canonical decompositions/combining classes")
    push("// for assigned characters, so a newer Unicode version only appends entries.")
    push("// Consumed exclusively by src/editor/serializer/src/nfc.cpp (anonymous namespace).")
    push("")
    push(f'inline constexpr char kNfcUnicodeVersion[] = "{unicodedata.unidata_version}";')
    push("")

    push("struct CccEntry")
    push("{")
    push("    char32_t cp;")
    push("    std::uint8_t ccc;")
    push("};")
    push(f"inline constexpr CccEntry kCccEntries[{len(ccc)}] = {{")
    for row in _chunk([f"{{0x{cp:X}, {c}}}" for cp, c in ccc], 6):
        push("    " + ", ".join(row) + ",")
    push("};")
    push("")

    push("struct DecompEntry")
    push("{")
    push("    char32_t cp;")
    push("    std::uint32_t offset;")
    push("    std::uint32_t length;")
    push("};")
    push(f"inline constexpr DecompEntry kDecompEntries[{len(decomp_entries)}] = {{")
    for row in _chunk([f"{{0x{cp:X}, {off}, {ln}}}" for cp, off, ln in decomp_entries], 5):
        push("    " + ", ".join(row) + ",")
    push("};")
    push(f"inline constexpr char32_t kDecompData[{len(decomp_data)}] = {{")
    for row in _chunk([f"0x{cp:X}" for cp in decomp_data], 10):
        push("    " + ", ".join(row) + ",")
    push("};")
    push("")

    push("// Key packs (first << 21) | second — both codepoints are <= 0x10FFFF (21 bits).")
    push("struct CompEntry")
    push("{")
    push("    std::uint64_t key;")
    push("    char32_t composed;")
    push("};")
    push(f"inline constexpr CompEntry kCompEntries[{len(compositions)}] = {{")
    comp_rows = sorted(((first << 21) | second, cp) for first, second, cp in compositions)
    for row in _chunk([f"{{0x{key:X}ULL, 0x{cp:X}}}" for key, cp in comp_rows], 4):
        push("    " + ", ".join(row) + ",")
    push("};")
    push("")

    push("// Inclusive ranges of cps whose NFC quick-check is No or Maybe (conservative union).")
    push("struct QcRange")
    push("{")
    push("    char32_t lo;")
    push("    char32_t hi;")
    push("};")
    push(f"inline constexpr QcRange kQcNoOrMaybe[{len(qc_ranges)}] = {{")
    for row in _chunk([f"{{0x{lo:X}, 0x{hi:X}}}" for lo, hi in qc_ranges], 6):
        push("    " + ", ".join(row) + ",")
    push("};")
    push("")

    text = "\n".join(lines)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return len(text)


def _chunk(items: list[str], n: int):
    for i in range(0, len(items), n):
        yield items[i : i + n]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--out",
        default="src/editor/serializer/src/nfc_tables.inc",
        help="output path (default: the committed include file, relative to the repo root)",
    )
    args = parser.parse_args(argv)
    size = emit(args.out)
    print(f"wrote {args.out} ({size} bytes, Unicode {unicodedata.unidata_version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
