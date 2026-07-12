# `samples/` — the agent few-shot corpus (R-QA-006)

Maintained, runnable Context sample projects that exercise the **breadth** of the public contract
surface. This is the **agent / benchmark** few-shot set (learning-by-example for an AI author),
distinct from the human-onboarding set (R-HUX-010). It is the prerequisite the ROADMAP (§1, M3 exit)
names for the owner-gated contract freeze (R-CLI-004, `protocolMajor` `0`→`1`): the corpus must have
**exercised the surface** before the freeze.

> This corpus does **NOT** perform the freeze — `protocolMajor` stays `0`. It is the freeze-*readiness*
> artifact.

## Maintained — the corpus rots the build if it breaks

`samples/` is wired into CI as the blocking **`samples-corpus`** gate (R-BUILD-009 / R-QA-008), run in
the `build` job on all three OS legs (`ctest --preset dev -R "^samples-corpus"`;
`src/tests/integration/test_samples_corpus.cpp`):

- **Leg A — build + headless smoke.** The REAL shipped `context` binary runs `validate` and
  `migrate --dry-run` over each project across a true process boundary. A malformed or drifted sample
  turns the gate red.
- **Leg B — contract-surface coverage.** The corpus is driven through the breadth of the stable,
  implemented, one-shot CLI verbs in the ONE registry (R-CLI-009). Because every surface (CLI ≡ RPC ≡
  MCP ≡ introspection) is a pure projection of that registry, exercising a CLI verb certifies its RPC
  method + MCP tool by construction. The gate asserts every such verb is exercised (save a small,
  documented exemption set) and every registered file **kind** is represented.

The authored JSON is a **canonical fixpoint** (`context migrate --dry-run` reports 0 changes), so a
non-canonical edit is caught.

## Projects

### `platformer-2d/`
A tiny 2D platformer. Startup scene composes a shared room sub-scene (L-35) and adds a camera; the
instanced torch carries a **field override** with a recorded `base` (R-CLI-006 override hygiene).
Kinds: `ctx:project`, `ctx:scene` (+ `camera`/`transform` components), `ctx:tilemap`,
`ctx:string-table`. Plus `scripts/movement.ts` — a TypeScript gameplay system authored as a
`(query, executor)` pair (R-LANG-002/009/010), the SHAPE an agent should emit (TS *evaluation* is
covered by the runtime's own suite; this corpus exercises the file-authoring surface).

### `topdown-rpg/`
A tiny top-down RPG. Startup scene applies the three **structural override** kinds (remove / add /
pointer, L-35). Ships a three-way **merge corpus** under `merge/`: `clean/` (field-disjoint edits that
converge with no conflict) and `conflict/` (same-field edits that yield a machine-readable conflict
`resolve-conflict` clears — R-FILE-012). Kinds: `ctx:project`, `ctx:scene`, `ctx:tilemap`,
`ctx:string-table`.

### `replays/`
`demo.replay.json` — a recorded, deterministic headless run (the `ctx:replay` kind, R-QA-005 / L-54):
seed + input stream + tick count + the expected per-tick state-hash trace. Deterministic and
project-manifest-free, so it replays green on every OS.

### `anim-graphs/`
`locomotion.anim-graph.json` — an authored animation state-machine (the `ctx:anim-graph` kind,
R-SYS-008): named states each playing a DCC-imported clip, connected by transitions gated on a control
parameter. The animation package (`src/packages/animation/`) compiles it into a runtime graph it
evaluates deterministically, blending the active clip's root motion into the entity transform. A
standalone kind sample (like `replays/`) — not part of a runnable project.

## Contract-surface coverage (freeze readiness)

Stable, implemented, one-shot CLI verbs the corpus exercises (each ≡ its RPC method + MCP tool):

| verb | corpus exercise |
| --- | --- |
| `describe` | the whole self-describing contract |
| `new` | scaffold the R-QA-006 runnable default template, then `validate` it |
| `validate` | each sample project + the scaffolded project |
| `migrate` | canonical-fixpoint dry-run over a sample |
| `set` | composed field override (id-path, records `base`) — platformer |
| `query --overrides` | one-shot override-hygiene read — platformer |
| `merge-file` | clean + conflicting three-way merges — topdown `merge/` |
| `resolve-conflict` | clear every conflict entry to empty |
| `session new/seed/inject/step/hash/record` | a startable headless session + steps |
| `replay` | replay the committed `ctx:replay` artifact |
| `determinism diff` | triage two differently-seeded runs (R-QA-005 / L-54) |

**Documented exemptions** (stable+implemented, but need a live-daemon/lockfile fixture the
file-authoring corpus cannot supply; each covered by its own unit test):

- `resource read` (`fetch`) — needs a live daemon-issued opaque resource handle (`test_fetch_e2e`).
- `install` — needs a `package.json` + `package-lock.json` + offline artifact cache with real SRI
  (`test_pkg_command`).
- `re-key` — remediates a genuine duplicate-intra-file id, a state the canonical serializer + merge
  normally prevent (an add/add id collision surfaces as a *resolvable conflict*, exercised above);
  raw remediation is covered by the `src/editor/merge` unit tests.

Operational (daemon-served) verbs are `stability != "stable"` and reserved verbs are
`implemented == false`, so both are outside the freeze-relevant one-shot surface by construction.
