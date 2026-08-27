---
id: e05b-manifest-roster-state-contract
title: editor-core (b) — panel manifest v2, ExtensionRegistry roster promotion, a11y regeneration, D6 state contract, render_html hardening
group: A
sequence: 3
repo: "."
base_branch: "main"
depends_on: [e04-window-shell-windows]
importance: 9
complexity: 8
security_critical: true   # carries a BREAKING contract-major bump + the XSS escaping contract
production_touching: false
model_hint: top
taskflow_refs: [04, 02, 05]
split_from: e05-editor-core-foundation   # owner-approved decomposition 2026-07-20
---

> **Split from [`e05-editor-core-foundation.md`](e05-editor-core-foundation.md)** (owner ruling
> 2026-07-20). This is the **pure-C++** half — fully verifiable on a local host, and deliberately
> separated because it carries a **BREAKING contract-major bump** that deserves its own reviewable
> PR. **May run CONCURRENTLY with e05a** (disjoint trees: C++ contract/registry vs TS webui).

## Goal

Land the C++ contract and registry work editor-core needs: panel manifest v2, promotion of
`ExtensionRegistry` to the single global roster, mechanical regeneration of the a11y hand-list,
the D6 panel state contract, and the `render_html` escaping hardening. No TS, no CEF.

## Scope & seams

- **Panel manifest v2** (04 §3): extend `Contribution` (`extension.h:32-44`) — contractVersion 2,
  icon, dock defaults, content type (`uitree|iframe`), state schemaVersion, capabilities, commands,
  themes.
- ⚠ **BREAKING `kContractMajor` 1→2.** The registry is deny-by-default and its compatibility window
  is **exactly `{kContractMajor}`** (`extension.h:16-20`), so the bump rejects every v1
  contribution the moment it lands. Ripples through **`editor_host.cpp:186-195`**,
  **`test_m5exit3_seam_checklist.cpp`**, **`test_registry.cpp`** — update all of them in the same
  PR. **Enumerate EVERY in-repo consumer before flipping** (the e02 lesson: its spec assumed "the
  CLI is the only client", missed the in-repo `RpcClient` harness, and reddened all five
  `m1-exit-*` gates).
- **Roster promotion**: `ExtensionRegistry` becomes the single global roster. Today
  `editor_host.cpp:184` builds a **stack-local** one — promote it; deny-by-default stands.
- **a11y regeneration**: regenerate the hand-maintained a11y list (`a11y/registry.cpp:21-113`)
  **FROM** the roster (mechanically enforced, not hand-copied) and **ADD `builtin.session.undo`**
  (A-F2 — absent from both current anchors).
- **State contract (D6)**: `getState()/restoreState()` versioned blobs on every panel; purity rule
  panel = f(bridge state, blob); schemaVersion mismatch → `null` + diagnostic (never a crash).
- **Escaping contract (C-F6)**: `render_html` mandatory escaping on EVERY text interpolation,
  T1-asserted with adversarial project strings. This is the XSS-from-project threat row — CSP is
  only the backstop, escaping is the control.

## Definition of Done

- [ ] Manifest v2 lands with `kContractMajor` 1→2; all three named ripple sites updated; every
      in-repo consumer enumerated and migrated (harnesses/fixtures included)
- [ ] `ExtensionRegistry` is the single global roster; deny-by-default preserved
- [ ] a11y list mechanically regenerated FROM the roster (a hand-edit cannot drift it);
      `builtin.session.undo` covered
- [ ] D6 state contract on every panel; schemaVersion mismatch → `null` + diagnostic (T1)
- [ ] `render_html` escapes every interpolation; T1 adversarial-string suite green
- [ ] 3-OS CI green incl. sanitizer legs; all five `m1-exit-*` gates still green after the bump
