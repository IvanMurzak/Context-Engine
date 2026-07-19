# Cross-implementation canonical-serialization test-vector corpus — RESERVATION (R-FILE-001)

> Design authority: `.claude/design/context-engine/core/REQUIREMENTS.md` **R-FILE-001** (canonical serialization) +
> **R-QA-011** (test corpora as versioned deliverables). This document **reserves the format and
> location** of the corpus so the M2 canonical serializer lands its vectors into a pre-agreed home.
> **No vectors exist yet** — this is a reservation, not the corpus. Populating it is an M2 deliverable
> (the serializer that produces canonical output is M2; there is nothing to fixpoint-test until then).

## Why reserve it now

R-FILE-001 mandates that canonical serialization ship a **cross-implementation test-vector corpus**
that **every** writer implementation (the C++ writer and the TS writer) reproduces **byte-exactly**.
The M0 parse-bench spike already proved (3,810 files, 0 mismatches) that the ECMAScript
`Number::toString` rules are only cross-language byte-stable **via fixpoint testing** — so the corpus
is **load-bearing, not a formality**. Reserving the location + format at M1 (alongside the
fault-injection harness) means the M2 serializer task drops vectors into a settled contract rather
than inventing one under deadline.

## Reserved location

```
tests/corpora/canonical-vectors/          <- repo-root, versioned, committed (R-QA-011 deliverable)
├── corpus-manifest.json                   <- version + vector index + per-category counts
├── float/                                 <- float edge cases
├── nfc/                                    <- Unicode NFC normalization cases
└── ordering/                              <- key-sort + id-keyed array ordering cases
```

`tests/corpora/` is the reserved parent for **all** R-QA-011 corpora (migration fixtures, three-way
merge, malformed-file, importer fuzz seeds, composition edge-cases); the canonical-serialization
vectors are the first tenant. This is **distinct** from `bench/corpora/` (gitignored, generated
benchmark inputs) — test-vector corpora are **committed** deliverables, benchmark corpora are not.

## Reserved format

Each vector is a pair — an **input** and its **golden canonical output** — plus the property the
writer must satisfy:

- `<category>/<NNN>-<slug>.in`  — the input bytes (authored form).
- `<category>/<NNN>-<slug>.canonical` — the golden **byte-exact** canonical serialization.
- Properties every writer asserts against the pair:
  1. **`serialize ∘ parse` idempotence** — parsing the input then serializing yields `.canonical`.
  2. **Re-canonicalization fixpoint** — canonicalizing `.canonical` is a no-op (byte-identical).
  3. **Cross-implementation identity** — the C++ and TS writers both emit `.canonical` byte-for-byte.

`corpus-manifest.json` (reserved shape):

```json
{
  "corpus_version": 1,
  "requirement": "R-FILE-001",
  "categories": {
    "float":    { "count": 0, "note": "shortest-round-trip boundaries, -0, subnormals, fixed vs exponent notation" },
    "nfc":      { "count": 0, "note": "NFC normalization cases; ASCII quick-check fast-path fixtures" },
    "ordering": { "count": 0, "note": "canonical key-sort + id-keyed array ordering (L-33: arrays of id-carrying objects, NOT map-keyed)" }
  },
  "vectors": []
}
```

## Category coverage the M2 corpus MUST carry (from R-FILE-001)

- **float** — shortest-round-trip boundaries, `-0` → `0`, subnormals, and the fixed-vs-exponent
  notation boundaries the parse-bench spike flagged as the subtle cross-language divergence.
- **nfc** — Unicode NFC normalization cases, plus fixtures that exercise the **ASCII quick-check fast
  path** the M1 serializer must implement (NFC cost is unmeasured as of M0 — M2 measures it).
- **ordering** — canonical key-sorting for object members, and **id-keyed child collections realized
  as arrays of objects carrying a stable `id`** (L-33 — the map-keyed encoding is forbidden, so
  authored array order is preserved and merge identity comes from the `id` member).

## Ownership + versioning (R-QA-011)

The corpus is a **versioned, committed deliverable with an owner**, not an incidental fixture. Every
addition bumps `corpus_version`; vectors are **kept forever** (a golden that is deleted stops
protecting the byte-format it pinned). The M2 serializer PR that first emits canonical output is the
PR that populates this corpus and wires the three property assertions above into `ctest` (C++ writer)
and the TS writer's suite.
