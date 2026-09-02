---
id: "e2-files-panel-write"
title: "Files panel write half: rename / move / delete through the L-30 write path, with undo and loud refusals"
group: "E"
sequence: 2
repo: "."
base_branch: "main"
depends_on: ["e1-files-panel-read"]
importance: 7
complexity: 8
security_critical: true
production_touching: false
model_hint: "top"
taskflow_refs: ["02-target-architecture.md", "06-viewport-and-files.md", "08-compatibility-and-migration.md"]
---

## Goal

Implement D10's write half: the Files panel becomes a full file manager — rename, move, **delete** —
through the one L-30 write path, with undo and loud refusals. Rename/move ride the **existing**
engine operations; **delete is a genuinely new engine operation** (verified: `asset_database.h`
exposes no delete — its only removal is the internal `"meta-residue-removed"` step — and the
contract registry carries only `asset move` (`registry.cpp:341`) and `asset rename` (`:350`)).

**⚠ OWNER GATE: this task's PR is not merged until the owner has approved the delete semantics and
seen the refusal + undo-restore evidence in the PR body.** D10 ratified the feature; the concrete
delete semantics were left to be sized here, and destructive file operations on a user's project get
a human eye before merge.

## Scope & seams

- **Rename / move**: over `AssetDatabase::move_asset` and the `asset move` / `asset rename` verbs,
  which already implement the R-FILE-004 dependency-safe order (destination file, destination meta,
  then source removal — GUID identity survives every observed mid-state), idempotence under partial
  apply, and **refusal on an occupied destination, never overwrite**. The panel adds the authoring
  surface only.
- **The authoring surface**:
  - the `file_write` capability declared in the panel's manifest — never ambient;
  - the **one L-30 write path** — the panel does not write files itself;
  - undo journal entries, so every operation is reversible like any other authored mutation;
  - **loud refusals** on the existing `editor.ui.write-notice` topic with the correct kind token.
    The three tokens are **`drop`**, **`refusal`**, **`abandoned`** (`write_notice.h:108-110`),
    byte-compared against `WRITE_NOTICE_KIND_*` by `tools/check_webui_assets.py --panel-contract`
    (`:413-422`) — **do not mint a fourth**. A refused rename (nothing written) is `refusal`; a value
    that moved under the edit is `drop`. ⚠ `bad`/`wait` are notification **TONES** that
    `notifications.ts:142` derives from the kind (`drop`/`abandoned` → `wait`, else → `bad`) — the
    tone is free and must never be put on the wire.
- **The delete sub-scope, sized** — mint the engine operation and its contract verb with the move
  path's discipline (dependency-safe order, idempotent under partial apply, refuse rather than
  clobber). Three decisions this task takes and records in the PR body and the verb's contract
  description:
  1. **Removal order** for the asset file + its sidecar meta such that an interrupted delete is
     completable/recoverable on the next scan — mirror the `"meta-residue-removed"` precedent.
  2. **References**: where the assetdb index can see referrers, the discipline-consistent default is
     **refuse with a diagnostic naming them**; where it cannot (the index reads sidecars, never
     payloads), deletion leaves a dangling reference surfaced by the engine's existing missing-asset
     diagnostics. The PR documents which applies and why.
  3. **Undo restore**: undo of a delete restores the file **and** its meta byte-identically. The
     mechanism (journaled payload vs. quarantine-aside) is the implementer's choice, proven by a
     round-trip test.
- Out of scope: recursive folder delete UX beyond what the verb's semantics define; trash/recycle-bin
  OS integration; any change to the three write-notice kinds or the `GestureVerb` set (both frozen,
  `08` §4).

## Definition of Done

- Rename and move work end to end from the panel through the L-30 path; an occupied destination is
  **refused** with a `refusal` write-notice (asserted on the wire with the kind token, and the UI
  tone derived, not sent).
- Delete verb tests: happy path (file + meta gone, index updated); refusal path per decision 2 with
  its diagnostic; **idempotence under partial apply** (re-running a half-applied delete completes
  it); an interrupted delete is recoverable per decision 1.
- **Undo round-trip**: delete then undo restores file and meta byte-identically (content hash
  compared); rename/move undo restores the prior path.
- Grant enforcement, both halves: without `file_write` the operations are refused; with it they
  proceed.
- Every "X did not happen" claim carries a producible sibling (e.g. the refusal test has a sibling
  proving the same fixture deletes when unreferenced).
- `check_webui_assets.py --panel-contract` green (no new kind token); undo journal entries visible in
  Session History.
- **Owner approval recorded on the PR before merge** (the gate above).
- Tests in the same PR (R-QA-013); PR body cites D10 and R-FILE-004.
