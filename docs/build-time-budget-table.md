# Build-time budget table (R-BUILD-006 — M8 a12)

> Design authority: `.claude/design/context-engine/core/REQUIREMENTS.md` **R-BUILD-006** (committed build-time budgets),
> **R-QA-009** (perf-gate methodology), **R-QA-012** (fleet-manifest advisory-until-provisioned),
> **L-28** (hybrid derivation cache / cache-exempt LTO-link + per-build costs), **L-42** (from-source
> vcpkg), ROADMAP §1-M8 a12. This document is the **normative allocation + justification** of the
> committed build-time budgets. The machine-readable copy CI consumes is `bench/build-time-budget.json`;
> the runnable harness + gate is `bench/build_time.py`.

## Why two halves, budgeted separately

C++ has no Cargo — ABI hell and build times are the direct cost of choosing C++ (the risk table).
The mitigation is vcpkg manifest-mode **from-source** package distribution (L-42) + one pinned
toolchain + the **L-28 shared content-addressed cache**, so the expensive from-source compile costs
**one build per machine ever**. But two costs are **NOT** amortized by that cache and recur on
**every** build — they must be their own budget lines, never hidden inside the compile average:

| budget line | cache-exempt? | task | what it is |
|---|---|---|---|
| **from-source C++ compile** | no (amortized) | L-42 | the vcpkg from-source ports + engine TUs, cached once (L-28). Measured **warm** (default) and **cold** (worst case). |
| **per-platform transcode** | **yes** | a03 | transcoding each asset to the target's optimal format, over the v1 platform set — paid every build. |
| **LTO/DCE final link** | **yes** | a05 | the R-KERNEL-003 generated-registration TU + uniform LTO across from-source ports + linker GC — the LTO final link recurs every build even when every input object is cached (R-BUILD-006 states it explicitly). |

## Warm is the default; cold is the tracked worst case

Per R-BUILD-006, **hermetic CI seeds a trusted read-only warm remote cache so cold-CI is not the
default**. The CI benchmark therefore measures the **WARM remote-cache-assisted** path as the default
(only the changed delta recompiles + relinks), and the fully **COLD** path (no cache — a fresh
machine or a cache miss) is the **tracked worst case**. The warm/cold split lives on the
`from-source-compile` line only; the two cache-exempt lines are cache-state-independent by
construction.

## The committed budgets

The numbers below are the **initial v1 commitments** (`bench/build-time-budget.json`). The design
docs commit to the *discipline* (committed cold/incremental/clean-CI budgets, cache-exempt costs
budgeted separately) but name no numbers, so this table **is** the committed allocation — revised
only by a reviewed PR, and ratified when the perf-isolated runner class is provisioned.

| category | budget | rationale |
|---|---|---|
| `from-source-compile` (warm) | **≤ 300 s** | incremental / remote-cache-assisted engine build — only the changed delta recompiles + relinks (R-BUILD-006 "incremental" + "clean-CI warm-cache"). |
| `from-source-compile` (cold) | **≤ 2400 s** | fully cold from-source build with no cache; the tracked worst case (the L-28 cache makes this a once-per-machine cost). |
| `transcode` (a03) | **≤ 30 s** | per-platform asset transcode over the v1 platform set — cache-exempt, per build. |
| `lto-link` (a05) | **≤ 180 s** | LTO/DCE final link — cache-exempt, per build. |

The comparison band is **±10%** (the R-QA-009 minimal-v1 band), applied as a one-sided breach test:
a category is **over** only when its measured median exceeds `budget · 1.10`. Faster than budget is
never a breach (a committed budget has no rolling-baseline "improved → rebaseline" signal — the
budget is the contract).

## Deferred to v2 (with iOS) — tracked, never silently green

R-BUILD-006 defers two budget lines to **v2 with iOS**; they are **NOT** budgeted in v1 but are
listed as `v2-pending` rows by the harness so they stay visible:

- **WASM→native AOT** — no v1 target requires WASM→native AOT (iOS moved to v2, consoles are v1-WON'T),
  so the AOT toolchain spike + its budget line land with v2's first (iOS) deliverable (R-LANG-005).
- **JS-VM bytecode-precompile** — the hermesc/qjsc-class precompile follows iOS to v2 (L-61's
  constrained-target VM), its R-BUILD-006 budget line moving with it.

## Advisory until the perf box is provisioned (R-QA-009 rule 1 / R-QA-012)

R-QA-009 rule 1 requires a **named, perf-isolated runner class** — never a shared CI box. The
build-time budgets map to the **`perf-linux-bare-metal`** runner class (`docs/ci-fleet-manifest.json`),
which is **not yet provisioned** (ops1 — an open owner hardware gate). So:

- the **per-PR CI benchmark job runs green end-to-end** (the harness works + archives a time series),
  but its **numeric budget gate is advisory** (`continue-on-error`) — a breach is reported +
  archived, never a red-X, exactly like the sibling `bench-baseline` / `bench-attach-10k` perf gates;
- the per-PR proxy runs on `gh-ubuntu-shared` + a warm cache (a shared runner is not the named perf
  box, so its numbers are advisory even before the perf box exists);
- the **blocking flip is a future step gated on ops1** — when the perf box is provisioned, set the
  runner class `provisioned: true` in the fleet manifest and drop `continue-on-error` from the gate
  step.

## Running it

```bash
# Measure the warm path (median-of-5), isolating the LTO/DCE link with a reset that rm's the binary:
python3 bench/build_time.py measure --cache-mode warm --runs 5 \
    --runner-class gh-ubuntu-shared --label ci-warm-ubuntu \
    --phase "from-source-compile::warm-rebuild::cmake --build src/build/dev --target context_runtime_server" \
    --phase "lto-link::runtime-server-link::cmake --build src/build/dev --target context_runtime_server" \
    --reset "lto-link::rm -f src/build/dev/runtime/host/context-runtime-server" \
    --phase "transcode::linux-server::src/build/dev/cli/context build --target linux --flavor server --project <proj> --out <pack> --runtime <bin>" \
    --out bench/results/build-time-ci-warm

# Gate the medians against the committed budget within the +/-band, archiving the time series:
python3 bench/build_time.py gate --result bench/results/build-time-ci-warm.json \
    --budget bench/build-time-budget.json --archive bench/results/archive \
    --out bench/results/build-time-table   # add --strict once the perf box is provisioned

# Band-math + v1/v2-category self-check (no build needed):
python3 bench/build_time.py --selftest
```
