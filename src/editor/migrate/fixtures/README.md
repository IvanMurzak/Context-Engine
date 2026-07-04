# Migration fixtures (R-QA-011 — versioned deliverables, kept forever)

One directory per component-type family, one subdirectory per schema bump:

```
fixtures/<family>/v<FROM>-to-v<TO>/
├── input.json    — a pre-migration document, stamped v<FROM> in componentVersions
└── golden.json   — the byte-exact CANONICAL post-migration output at v<TO>
```

`tests/test_fixtures.cpp` enumerates every pair, migrates each `input.json` under a set whose
current version is `<TO>` (with the bulk-path `stamp_registered_sites` semantics — the same
transform `context migrate` writes to disk), and byte-compares the canonical serialization against
`golden.json`. It then re-migrates each golden and asserts the idempotence fixpoint (a second
migration is a no-op).

Rules (R-QA-011):

- **A fixture pair is a deliverable of the schema bump itself** — added in the SAME PR that
  registers the migration step, never later. If the fixture is not part of the bump, it never
  exists afterwards.
- **Pairs are kept forever.** They pin the step's behavior for as long as the step exists (steps
  are write-once; a behavior change bumps the step's `revision` and updates goldens deliberately —
  a diff on a golden is a reviewed act, never a drive-by).
- **Multi-step families also pin the full chain**: a `v1-to-v3`-style pair proves chaining, path
  transforms (override rewrites + orphan preservation), and re-stamping end to end.

The `test-sprite` family exercises the reference steps registered in `tests/migrate_test.h`
(`test:` namespace — deliberately NOT production steps: the engine-shipped set,
`MigrationSet::engine_set()`, is empty until the first real engine component-schema bump).
