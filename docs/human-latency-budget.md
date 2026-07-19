# Human-interaction latency budget — R-HUX-011 (M5 exit criterion, SHOULD)

The **human-interaction latency budget** is an M5 exit criterion of **R-HUX-011** (.claude/design/context-engine/core
`REQUIREMENTS.md` § R-HUX-011; ROADMAP §1 M5). It is the human-loop counterpart of the M1
per-stage **agent/scale** budget table (`docs/latency-budget-table.md`, R-FILE-011): a human notices
interaction latency at a different (perceptual) threshold than the agent-throughput targets, so the
human loop gets its own committed budget or the editor is "fast for agents, laggy for people."

**Status: SHOULD — MEASURED, not a hard gate.** R-HUX-011 is a continuous `SHOULD` target, not a
blocking M5 threshold (ROADMAP §1 M5: "R-HUX-011 … measured from instrumented timestamps … are
continuous SHOULD targets, not hard M5 gates"). The M5 exit obligation is that the loops are
**measured from instrumented timestamps in the real event path** (R-EDIT-001) and **recorded** — this
document is that record; the numbers themselves do not red-X the rollup.

## The three human-interaction loops

R-HUX-011 names three core interactive loops. Each is a **listener seam** the observer-editor panels
ship in their headless (CEF-free) logic tier, so the loop can be driven + timed WITHOUT booting CEF;
the CEF host wraps the SAME seam with the real input→paint timestamps.

| loop | what it is (M5 observer surface) | the instrumented seam | where it is measured |
|---|---|---|---|
| **selection** | click/keyboard-select a scene-tree row → the inspector + other panels react | `scenetree::SceneTreePanel::add_selection_listener` (fires `SceneSelection` on `select`) | `m5-exit-1-walkthrough` around `tree.select(...)`; the CEF host times select→inspector-paint |
| **gesture → viewport update** | a viewport re-frame gesture → the observer view repaints | `viewport::ViewportPanel::add_view_update_listener` (fires `ViewportUpdate` on `frame_scene`) | `m5-exit-1-walkthrough` around `vp.frame_scene()`; the CEF host times input→paint |
| **inspector commit** | gesture-end commit of an inspector field edit → the override write lands + panels react | `inspector::InspectorPanel::add_commit_listener` (fires `CommitResult` on `commit`) | `m5-exit-1-walkthrough` around `ipanel.commit()`; the CEF host times input→commit→derive |

## Budget allocation (initial M5 commitment — perceptual thresholds)

The budgets below are the **initial M5 allocation** — perceptual targets, revisable through the
R-QA-009 rolling-baseline discipline as the shipped CEF surface + real derive/paint path land. They
are **SHOULD** targets: the gate is that the loops are measured + recorded, not that they beat a number
on a shared CI runner (which is not the R-QA-009 perf-isolated runner class — the same
advisory-until-provisioned contract the R-FILE-011 bench numbers carry).

| loop | budget (p95, warm) | rationale |
|---|---|---|
| selection | ≤ 50 ms | selection is a read-only re-projection (no write, no derive) — should feel instant (the classic "100 ms feels instant" perceptual floor, halved for headroom). |
| gesture → viewport update | ≤ 16 ms (one 60 Hz frame) | the observer viewport is a per-frame surface; a re-frame must land within one frame to read as continuous (R-PLAY / R-REND frame pacing). |
| inspector commit | ≤ 100 ms | a commit crosses the write path (CAS-guarded override write) + a re-derive + a repaint; ≤ 100 ms keeps the edit→see-it loop within the "feels instant" perceptual window. |

## Where the measurement runs

The `m5-exit-1-walkthrough` ctest (`src/tests/integration/test_m5exit1_walkthrough.cpp`, run by the
"M5 exit gate" step on every `build` leg) drives the three loops on the **real event path** — the
composed derived world → the selection event → the CAS-guarded inspector commit → the viewport
re-frame — and measures each from `std::chrono::steady_clock` timestamps captured immediately around
the loop's own trigger (NOT a synthetic benchmark; R-EDIT-001). It **records** the readings to stdout:

```
[m5-exit] R-HUX-011 human-loop latency (measured, SHOULD; ns): selection=<n> gesture->viewport=<n> inspector-commit=<n>
```

The gate asserts each loop was **measured** (a real, non-negative reading) and its **instrumentation
seam fired** (the listener was invoked) — the R-HUX-011 "measured from instrumented timestamps in the
real event path" obligation — never that it beat a threshold. When the shipped CEF host wires the same
seams to real input→paint timestamps over the live derive/paint path, this table becomes the
comparison baseline for the R-QA-009 rolling measurement (re-baselined per run, like the R-FILE-011
per-stage table), and the perf-isolated human-loop runner class provisioning is the point at which the
numbers could be promoted from advisory to gating.
