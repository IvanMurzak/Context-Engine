# Context Engine — task breakdown (M8 + M8.5, decomposed 2026-07-15)

Static, **immutable** task specs for the remaining v1 milestones. Live state (status, runs, PRs)
lives in **exactly one place**: [`../core/ROADMAP.md` §9 — waves + status board](../core/ROADMAP.md). Do
NOT add status to these files; do NOT edit them during implementation.

## Groups (merge-conflict domains)

| Group | Domain | Concurrency rule |
|---|---|---|
| **a** | `engines/context/Context-Engine` — ONE conflict domain (shared CLI registry, error catalog, `ci.yml`, CMake, fleet manifest) | **Strictly sequential in listed order** (a01 → a23). Also matches the owner's single-lane dispatch directive [2026-07-05]. |
| **ops** | Infra/operator work outside the repo (runner provisioning) | Parallel to group a; human-assisted. |

a01–a14 = **M8 (build pipeline)** — this folder · a15–a23 + ops1 = **M8.5 (wedge hardening)** —
[`../m85-wedge-hardening/`](../m85-wedge-hardening/) (split into sibling milestone folders
2026-07-18; this README remains the shared index for both).

## Coefficients

- **importance (1–10):** what breaks if this is wrong/missing (10 = milestone fails or security
  compromised).
- **complexity (1–10):** architectural depth + cross-cutting surface + correctness-cliff risk.

## Model-selection rubric → `model_hint`

| Rule | Tier |
|---|---|
| complexity ≥ 8 | **top** (Fable-tier) |
| complexity 5–7 | **mid** (Sonnet-tier) |
| complexity ≤ 4 | **fast** (Haiku-tier) |
| `security_critical` / production-touching | bump one tier |

**Dispatch reality (Context-Engine):** the owner's dispatch discipline [owner-ruled 2026-07-10]
is single-lane via the `implement-task` pipeline (`target=context-engine`) with **no per-step
model overrides** — `model_hint` is therefore advisory routing metadata, not a dispatch override.

## Human-approval gates (mirror of ROADMAP §9)

- **a08** — owner loads the minted private signing key (`.secrets/context-engine/`) into a
  GitHub **environment-protected secret** (custody model B) + approves the trust-root pin.
- **a10** — Windows signing cert procurement (Azure Artifact Signing subscription or a
  developer cert = real money).
- **a13** — owner approves loading the existing Apple Developer creds (`.secrets/apple-*`) into
  the engine repo's protected environment.
- **ops1** — perf-isolated runner hardware/hosting spend.
