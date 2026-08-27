---
id: e07a-webui-ts-test-tier
title: editor-core (07a) — webui TS T1 unit-test tier (esbuild-bundled, headless-Chromium ctest, 3-OS)
group: C
sequence: 19
repo: "."
base_branch: "main"
depends_on: [e05a-webui-workspace-toolchain]
importance: 8
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [09, 04]
split_from: e07-commands-palette-keymap   # owner ruling 2026-07-21
---

> **Split from [`e07-commands-palette-keymap.md`](e07-commands-palette-keymap.md)** (owner ruling
> 2026-07-21). e07 halted at `02-implement` **before writing code** — it is milestone-sized (~6
> independently-shippable DoD items across TS + a new C++ Shell bridge + a missing test tier + CI).
> Decomposed into a strictly-serial chain e07a→e07d, all sharing `src/editor/webui/` (mirrors the
> e05d1–e05d4 split). This is the **first** child and the foundational prerequisite: e07b/e07c/e07d
> all declare `T1-tested` DoD items that have **no test tier to run in today**.

## Goal

Build the **editor-core TS unit-test tier ("T1")** that the rest of e07 (and every later editor-core
TS task) needs: an esbuild-bundled test entry that runs under the **existing headless-Chromium CI**
and is registered as a `webui-*` ctest on the **3-OS matrix**, with **no new npm dependency**.

## Scope & seams

- **Why this exists (ground truth):** as of `Context-Engine@0761dc85` there is NO editor-core TS unit
  test tier — no TS test runner pinned, no `*test*`/`*spec*` TS files under `src/editor/webui/`; live
  TS is proven ONLY by the CI-only `editor-cef-smoke-shell` leg. The design's own ROADMAP (R-QA-013)
  flags this webui-test gap as un-started; e05d1's retrospective flagged it too. Close it here.
- **No new npm dep (hard constraint).** Reuse the e05a esbuild toolchain to bundle a test entry;
  execute the bundle in the headless Chromium the CI already provisions for `editor-cef-smoke`. A
  tiny in-repo assert/harness shim is fine; pulling a test-runner package is NOT (re-triggers the
  08 §3 supply-chain gate — the allowlist is `dockview-core@7.0.2` ONLY).
- **CI wiring is the load-bearing half.** The new ctest(s) MUST be (a) BUILT by the job's `--target`
  list AND (b) registered in the named `ctest -R` step. Miss either ⇒ "Not Run" ⇒ this project
  treats it as RED (CE #264). Verify from the ACTUAL CI job output, not by assuming registration.
- **Prove the tier with a real (small) unit test**, not an empty harness — e.g. a pure-TS module
  under `src/editor/webui/` with a couple of assertions, so a reviewer sees the tier actually
  executes assertions and FAILS red when they break (demonstrate with an intentionally-broken run).
- ⚠ **Toolchain seam (generalized from e05a — expect it):** tool paths published by `src/runtime/ts`
  are NOT visible from `src/editor/` (configured first); `tsgo` is not even `PARENT_SCOPE`-exported.
  Re-stage any such tool path locally or promote to `CACHE INTERNAL`.
- Out of scope: the command registry (e07b), keymap/Shell bridge (e07c), palette + T2 smoke (e07d).

## Standing lessons (carry forward — earned by e05a–e05d)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling test only exonerates a suspected flake if that leg actually registers + runs the
   affected code — confirm the platform gate (`if(OS_WINDOWS OR OS_LINUX)`-gated tests are Not-Run,
   not "passed", on macOS).
4. Known flakes CE #319 (`editor-cef-smoke-shell`) + #322 (`kernel_server` 0xc0000409). The e05d4
   self-hosted-Windows CEF infra flake (LocalSystem Session-0 GPU-denied + `post-build.bat`
   CEF-locale COPY file-lock) is env, not code — rerun/gate per precedent; any OTHER cause is REAL.

## Definition of Done

- [ ] A `webui-*` TS unit-test tier exists: esbuild-bundled test entry, run under the existing
      headless-Chromium CI, **no new npm dependency**
- [ ] The tier is registered as a ctest **BUILT by its job's `--target` list AND run in the named
      `ctest -R` step** — verified from actual CI output (Not-Run = RED)
- [ ] At least one real pure-TS unit test runs and asserts; an intentionally-broken run turns the
      ctest RED (demonstrated during development)
- [ ] `context_assert_shell_boundary` still passes non-vacuously; FORBIDDEN list untouched
- [ ] 3-OS CI green
