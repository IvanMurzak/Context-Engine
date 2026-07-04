# Canonical-serialization test-vector corpus

**Corpus version 1** (M2 wave 1, issue #42) — the cross-implementation test-vector corpus
R-FILE-001 mandates and R-QA-011 makes a **versioned committed deliverable**: every writer
implementation (this repo's C++ `src/editor/serializer/`, the future TS writer) must reproduce
every `*.expected.json` **byte-exactly** from its `*.input.json`. The corpus is load-bearing, not
a formality: the M0 parse-bench spike proved the ECMAScript number rules cross-language
byte-stable only via fixpoint testing (Python vs C++ writers, 3,810 files, 0 mismatches), and
these vectors carry that method forward as committed fixtures.

## File shapes

- `<name>.input.json` — arbitrary (often deliberately non-canonical) authored bytes.
- `<name>.expected.json` — the canonical serialization of the input: **exact bytes**, LF-only,
  ending with exactly one newline. Every expected file is also a **fixpoint** (canonicalizing it
  reproduces itself) — `test_vectors.cpp` asserts both directions.
- `<name>.diags` (optional) — one stable diagnostic code per line that canonicalizing the input
  must emit (e.g. `encoding.bom`, `encoding.crlf` — the non-fatal R-FILE-003 findings).

The sibling `.gitattributes` pins the whole tree `-text`: the bytes are the contract, so EOL
normalization on checkout must never touch them.

## Rules the vectors pin (R-FILE-001 / L-32 / L-33)

- **Formatting**: UTF-8, LF, 2-space indent, `": "` separator, one trailing newline.
- **Key order**: object members sorted lexicographically by UTF-8 bytes (`key-order-ascii`,
  `key-order-unicode`); **array order is authored order, never sorted**
  (`array-order-preserved` — id-keyed child collections are arrays of objects carrying `id`).
- **Numbers**: ECMAScript `Number::toString` shortest-round-trip for the double domain, with the
  fixed-vs-exponent boundaries (`floats-boundaries`: 1e20/1e21 and 1e-6/1e-7), subnormal and
  max-double edges (`floats-extremes`), and `-0` -> `0` (`minus-zero`). **Integer literals inside
  i64/u64 are preserved losslessly** (`ints-lossless`, incl. 2^53+1); integer literals **beyond
  u64 fall to the ECMAScript double domain** (`ints-beyond-u64`) — a TS writer using plain
  `JSON.parse` must take care to match the lossless i64/u64 rule (BigInt-aware parsing) for
  integers above 2^53; THIS corpus is the arbiter.
- **NFC**: every string (keys and values) is NFC-normalized (`nfc-latin`, `nfc-hangul` incl.
  algorithmic jamo composition, `nfc-reorder` canonical reordering + blocking, and
  `nfc-exclusions` — composition-excluded characters DECOMPOSE and stay decomposed).
- **Escaping**: minimal — control chars, quote, backslash; everything else raw UTF-8; `\u`
  escapes (incl. surrogate pairs) collapse to raw NFC UTF-8 on canonicalization (`escapes`).
- **Headers**: `$schema` + `version` + `componentVersions` sort like any other keys —
  `headers-scene` is a realistic scene document with id-carrying entities, a dual-form `$ref`,
  and a `$sidecar` reference.
- **Healing**: BOM and CRLF canonicalize away and surface as machine-readable diagnostics
  (`bom-crlf` + its `.diags`); whitespace noise erases (`formatting-noise`).

## Provenance & maintenance

Expected bytes were generated from the **Python reference writer** (`bench/gen_corpus.py`'s
`canonical_json` + `unicodedata` NFC + the pinned beyond-u64 rule) and verified byte-exact
against the C++ implementation — a genuine two-implementation cross-check. NFC tables:
Unicode 14.0.0 (see `tools/gen_nfc_tables.py`; the Unicode normalization stability policy keeps
existing canonical forms frozen across Unicode versions).

The corpus is **append-only per version**: add new vectors freely; changing EXISTING expected
bytes is a canonical-format change and requires a corpus version bump here plus a design-authority
check (R-FILE-001 is normative). Keep inputs deliberately noisy — the corpus proves
canonicalization, not pretty-printing.
