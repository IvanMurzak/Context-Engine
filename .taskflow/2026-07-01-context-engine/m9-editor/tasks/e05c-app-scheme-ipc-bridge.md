---
id: e05c-app-scheme-ipc-bridge
title: editor-core (c) — context-editor:// app scheme, resource handler, privileged Shell IPC bridge
group: C
sequence: 4
repo: "."
base_branch: "main"
depends_on: [e05a-webui-workspace-toolchain, e05b-manifest-roster-state-contract]
importance: 9
complexity: 8
security_critical: true   # privileged native↔JS channel + the token-isolation boundary
production_touching: false
model_hint: top
taskflow_refs: [04, 02, 03, 08]
split_from: e05-editor-core-foundation   # owner-approved decomposition 2026-07-20
---

> **Split from [`e05-editor-core-foundation.md`](e05-editor-core-foundation.md)** (owner ruling
> 2026-07-20). Separated because it is **from-zero CEF work that cannot be built on the dev host** —
> local signal is only pre-push-audit check 9 `-fsyntax-only`; real verification is CI-gated.
> Budget review time accordingly.

## Goal

Give editor-core a way to exist inside the Shell window and talk to it: the `context-editor://`
custom scheme + resource handler serving the e05a bundle, and the privileged native↔JS IPC bridge.

## Scope & seams

⚠ **Toolchain-seam trap (generalized from the e05a run — expect to hit it).** Tool paths published
by `src/runtime/ts` are **NOT visible from `src/editor/`**, because `src/editor/` is configured
BEFORE `src/runtime/ts`. This cost e05a real time on `CONTEXT_ESBUILD_BIN`, and **`tsgo` is
strictly worse** — not even `PARENT_SCOPE`-exported. Any such tool path must be re-staged locally
or promoted to `CACHE INTERNAL`; never assume a variable set in that subtree is readable here.

- ⚠ **Both halves are from ZERO.** `CefMessageRouter` has **zero hits repo-wide**; there is no
  `CefProcessMessage`/`CefRenderProcessHandler` under `src/editor/`; and production has **no**
  custom scheme (`docs/shell.md:312-315` explicitly defers it here). Expect to author the
  registration, handler, and process-side plumbing rather than extend existing code.
- **App scheme + resource handler**: static assets shipped in-app and served via
  `context-editor://app/…` with the pinned scheme flags — **never `file://` temp files**. Strict
  no-inline-script CSP; the scheme must be registered as standard+secure so CSP and origin
  semantics behave.
- **IPC bridge**: CefMessageRouter over `context-editor://ipc`. This is how editor-core reaches
  BOTH the daemon and the Shell (window registry, drag sessions, region maps).
- 🔒 **Token isolation is the point** (04 §1, 08): the **Shell** holds the daemon socket and the
  attach token; **editor-core MUST NEVER see either**. The bridge is the only path, and it is
  privileged — treat every inbound message as untrusted input from renderer-process content.
- Message envelopes carry the same shape as the daemon's so e05d's client can layer on cleanly.

## Definition of Done

- [ ] `context-editor://app/…` serves the e05a bundle under strict no-inline-script CSP; no
      `file://` fallback exists
- [ ] IPC bridge round-trips native↔JS over `context-editor://ipc` inside the e04 shell window
- [ ] Socket + attach token provably unreachable from editor-core (structural assert, not a comment)
- [ ] Malformed/hostile inbound bridge messages rejected without crashing the Shell (T1)
- [ ] CI-gated verification is green (this cannot be proven locally — do NOT claim a local pass);
      pre-push-audit check 9 `-fsyntax-only` run before push
- [ ] 3-OS CI green
