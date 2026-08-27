# M7 runtime-UI task specs — index

Static, **immutable** task specs for the M7 decomposition
([design](../../2026-07-13-m7-runtime-ui-decomposition.md), amended by the owner rulings
2026-07-13). **Live state (status/PR/dates) lives ONLY in [`../ROADMAP.md`](../ROADMAP.md)** —
never here.

## Coefficients

- **importance (1–10):** what breaks if the task is wrong/missing. 10 = milestone fails.
- **complexity (1–10):** architectural depth + cross-cutting surface + correctness-cliff risk.

## Model-selection rubric

| Rule | model_hint |
|---|---|
| complexity ≥ 8 | top (Fable-tier) |
| complexity 5–7 | mid (Sonnet-tier) |
| complexity ≤ 4 | fast (Haiku-tier) |
| security_critical / production-touching | bump one tier |

## Groups

ONE group **A** = the whole lane. Rationale: one repo (`engines/context/Context-Engine`) and
genuinely overlapping files (`src/packages/ui/`, `src/CMakeLists.txt`,
`src/render/ui/`, `ci.yml`, `goldens/`, `error_catalog.cpp`) — plus the standing owner rule that
CE tasks run **single-lane, 1-at-a-time** (2026-07-05/07-10). Tasks run strictly in listed order
a1 → a12; there are no parallel groups by design.

## Owner rulings folded in (2026-07-13)

- **(a)** v1 authoring = TS retained-tree API + CSS-like style props (a4); HTML/CSS-file
  fidelity arrives with the later optional CEF backend.
- **(b)** backends beyond null + engine-integrated are deferred trailing-v1/v1.x (a11 ships the
  conformance suite as the on-ramp).
- **(c)** text is FULL shaping-grade in M7 (HarfBuzz-class shaping + bidi) — T7 split into
  a7 (font substrate) + a8 (shaping). **Interpretation notes (both owner-vetoable):**
  (i) the recorded ruling text names SHAPING; **bidi is included by inference** from the
  R-UI-005 capability grouping ("полноценный shaping" read as rendering-side completeness);
  (ii) **IME remains a declared-`false` capability** in v1 — it is OS text-ENTRY integration,
  and no text-input widget is in the M7 exit scope. Also declared out: vertical text,
  justification, hyphenation (a8 lists them explicitly).
- **(d)** world-space RTT includes CURVED surfaces — T8 split into a9 (flat + dynamic-texture
  registry) + a10 (curved mesh UV mapping + raycast→UV→events).
