---
id: e05a-webui-workspace-toolchain
title: editor-core (a) — npm workspace, dockview supply chain, esbuild bundle, typecheck, JS-client codegen
group: C
sequence: 3
repo: "."
base_branch: "main"
depends_on: [s1-dockview-cef-spike, e02-client-sdk-boundary, e04-window-shell-windows]
importance: 9
complexity: 7
security_critical: true   # introduces the repo's first npm supply-chain channel
production_touching: false
model_hint: top
taskflow_refs: [02, 04, 05]
split_from: e05-editor-core-foundation   # owner-approved decomposition 2026-07-20
---

> **Split from [`e05-editor-core-foundation.md`](e05-editor-core-foundation.md)** (owner ruling
> 2026-07-20). e05 bundled four independently-shippable PRs; this is the **first**, chosen because
> it is the only one fully verifiable on a local host. Order: **e05a → e05b → e05c → e05d**
> (e05a ∥ e05b may run concurrently — disjoint trees).

## Goal

Stand up the TypeScript build substrate for editor-core: the `src/editor/webui/` npm workspace on
the repo's pinned toolchain, the SHA-pinned dockview supply channel, a build-time esbuild bundle
target (the repo's FIRST), typecheck, and the generated JS client typings. **No app behaviour
yet** — this is the toolchain that e05c/e05d build on, and it must be green and self-contained.

## Resume from preserved WIP

Commit **`5f942fe`** on **`origin/worktree-9be14dcd847c`** already delivers the dockview fetch
channel — `tools/dockview-toolchain.json`, `tools/fetch_dockview.py`, `tools/tests/test_fetch_dockview.py`
(563 insertions, 22/22 pytest green, verified against the live npm registry). **Start from it**
rather than re-authoring.

⚠ **Known trap:** `CONTEXT_ESBUILD_BIN` is NOT visible from `src/editor/` — `src/editor/` is
configured BEFORE `src/runtime/ts`. Either re-stage it locally or promote it to `CACHE INTERNAL`.

## Scope & seams

- **Workspace**: `src/editor/webui/core/` → `@context-engine/editor-core`.
- **Bundling**: build-time bundle via the EXISTING SHA-pinned esbuild (`tools/ts-toolchain.json`,
  `tools/fetch_esbuild.py`). **No Node at runtime; no npm in CI beyond pinned, fetch-verified
  tooling.** This is the repo's first build-time bundling target — wire it as a proper CMake target.
- **Dependency pin** ⚠: the owner's allowlist approval is **exactly one package —
  `dockview-core@7.0.2`** (MIT, 0 runtime deps), **version-pinned**. A bump past 7.0.2, or ANY
  additional `dockview-*` package (the design originally assumed a core+`dockview-modules` set;
  s1 disproved it), re-triggers the owner consent gate. Do not add either.
- **Typecheck**: wire the pinned typechecker over the workspace; failures are build failures.
- **JS client codegen**: typings generated from e02's build-generated schema (4221 lines).
  **Hand-written typings are prohibited** (R-CLI-009 spirit) — drift-check in CI.
- Fetch-verify fail-closed for every fetched artifact, matching the CEF/V8/esbuild precedent.

## Definition of Done

- [ ] `src/editor/webui/core/` builds a bundle through the pinned esbuild as a CMake target
- [ ] `dockview-core@7.0.2` fetched via a SHA-pinned, fail-closed channel; no other npm dep added
- [ ] Typecheck runs over the workspace and gates the build
- [ ] JS client typings generated from e02's schema; drift-checked in CI; no hand-written typings
- [ ] The `5f942fe` fetch-channel work is landed (not re-authored) with its 22 tests green
- [ ] 3-OS CI green; no Node required at runtime
