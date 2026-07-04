# src/editor/serializer — the canonical-JSON serializer (R-FILE-001, L-32/L-33)

The REAL canonical serializer (M2 wave 1, issue #42) that replaced the M1 whitespace placeholder
behind the derivation parse-node seam. Every authored file kind round-trips through ONE canonical
form: stable key order (UTF-8 byte sort), stable formatting (2-space indent, `": "`, LF, one
trailing newline), stable array ordering (authored order — id-keyed child collections are arrays
of objects carrying `id`; the map-keyed form is forbidden), ECMAScript `Number::toString`
shortest-round-trip number formatting (`-0` -> `0`; NaN/Infinity banned), and NFC Unicode
normalization for strings with an ASCII quick-check fast path.

## Layout

- `include/context/editor/serializer/`
  - `json_tree.h` — the dependency-free document DOM (lossless i64/u64 integers + ECMA doubles)
    and the R-FILE-003-shaped `Diagnostic`.
  - `json_parse.h` — strict RFC 8259 parser: unique keys, valid UTF-8, surrogate-pair-checked
    `\u` escapes, depth cap; BOM/CRLF are NON-FATAL diagnostics (`encoding.bom`,
    `encoding.crlf`) the canonical rewrite heals on save.
  - `nfc.h` — `is_ascii` (the fast path), `is_nfc_quick` (conservative UAX #15 quick check),
    `normalize_nfc` (full decompose -> canonical reorder -> compose; table-driven + algorithmic
    Hangul).
  - `canonical.h` — `ecma_number`, `serialize_canonical` (tree -> canonical file bytes),
    `canonicalize` (bytes -> canonical bytes + canonical hash + diagnostics; non-JSON passes
    through raw with raw ≡ canonical, the binary-sidecar rule), `canonical_hash_of` (FNV-1a 64),
    and the L-32 `DocumentHeader` reader (`$schema` / `version` / `componentVersions`).
- `src/nfc_tables.inc` — GENERATED Unicode tables (`tools/gen_nfc_tables.py`, Unicode 14.0.0).
- `tests/` — the ctest suites (below) + `vectors/`, the committed cross-implementation
  test-vector corpus (R-QA-011; see its README).

## The two hashes (R-FILE-001 split)

- **raw-byte hash** — `filesync::content_hash` over the bytes on disk: watch/reconcile change
  detection and CAS `--if-match`.
- **canonical-content hash** — `canonical_hash_of` over the canonical bytes: derivation/cache
  keys and the R-CLI-006 own-write barrier. Cosmetic byte differences never poison derivation.

Both are exposed, labelled (`rawHash` / `canonicalHash`), through the mutation-result envelope.
For non-JSON content (binary sidecars) there is no canonicalization pass, so raw ≡ canonical by
construction.

## NFC cost — measured (the spike-mandated number)

`serializer-test_nfc_bench` (8 × ~4 MiB, `steady_clock`; this table: Ninja + GCC 13.2 Release on
the Windows dev machine — AMD Ryzen 9 9950X; not an R-QA-009 isolated runner, trend-grade only):

| path | throughput |
|---|---|
| ASCII fast path (`is_ascii` scan) | ~5,000 MB/s |
| quick-check scan (NFC non-ASCII) | ~720 MB/s |
| `normalize_nfc` no-op (already NFC) | ~670 MB/s |
| FULL normalization (decomposed text) | ~52 MB/s |

Read: ASCII content — the overwhelming majority of authored bytes (keys, ids, refs, numbers) —
pays ~1/19th of the parse pipeline's own per-byte cost (parse-bench measured ~270 MB/s/core for
parse+build+canonicalize+hash), i.e. the fast path makes NFC effectively free where it does not
apply, and full normalization is paid only by strings that genuinely need rewriting. The quick
check is deliberately conservative: Maybe-class characters route through full normalization,
which is a byte-identical no-op on already-NFC text.

## Tests (R-QA-013)

`serializer-test_json_parse` (grammar + strictness + encodings), `serializer-test_ecma_number`
(notation boundaries + 4k-sample shortest-round-trip property), `serializer-test_nfc`
(composition/exclusions/Hangul/reordering/blocking + idempotence), `serializer-test_canonical_write`
(formatting shape, key sort, NaN/Inf ban, header reader), `serializer-test_vectors` (the
committed corpus, byte-exact + fixpoint), `serializer-test_property` (300 seeded documents:
serialize∘parse idempotence, re-canonicalization fixpoint, formatting-noise erasure),
`serializer-test_nfc_bench` (the measurement above; weak sanity asserts only — never a perf gate).
