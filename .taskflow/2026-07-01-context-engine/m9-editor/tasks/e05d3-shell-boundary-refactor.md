---
id: e05d3-shell-boundary-refactor
title: editor-core (d3) — D10 boundary refactor (split kernel-typed builders out) + live scenetree/inspector hydration
group: C
sequence: 8
repo: "."
base_branch: "main"
depends_on: [e05d1-panelhost-hydration-runtime]
importance: 9
complexity: 9
security_critical: true   # refactors the D10 boundary that makes D18 physical, not aspirational
production_touching: false
model_hint: top
taskflow_refs: [02, 03, 04, 05]
split_from: e05d-panelhost-hydration-layout   # owner ruling 2026-07-20
---

> **Split from [`e05d-panelhost-hydration-layout.md`](e05d-panelhost-hydration-layout.md)** (owner
> ruling 2026-07-20). This is the task the e05d halt was **about**. e05d stopped at `02-implement`
> before writing code because its DoD required Scene tree + Inspector + Problems to hydrate from the
> live daemon, while the D10 shell-boundary gate landed by e04 forbids exactly that for two of the
> three. **The owner has ruled. Read the ruling below before writing any code.**

## ⛔ OWNER RULING (2026-07-20) — binding

**Split BOTH kernel-typed builders out. Do NOT widen the gate's FORBIDDEN list.**

The cheap path — admitting `context_compose` to the Shell's link closure — is **rejected**.
`context_assert_shell_boundary` is what makes **D18** ("the editor app is a wire client only; the
ordinary-client guarantee is *physical*") true rather than aspirational. Widening it erases what
e04 deliberately landed. **Any diff that adds an entry to the FORBIDDEN list, removes a target from
`TARGETS`, or otherwise weakens the gate is out of contract for this task.**

## Ground truth (verified 2026-07-20 against the preserved e05d configure — do not re-derive from docs)

The gate lives at `src/CMakeLists.txt` (`context_assert_shell_boundary`) +
`cmake/ContextPresentIsolation.cmake`. It walks the FULL transitive closure (`LINK_LIBRARIES` +
`INTERFACE_LINK_LIBRARIES`, unwrapping `$<LINK_ONLY:>`) and **FATAL_ERRORs at CONFIGURE time on
every OS leg**. Current report (`src/build/dev/shell-boundary-report.txt`, preserved in worktree
`.claude/worktrees/1eeb21321ae4`):

```
FORBIDDEN-PRESENT context_editorkernel / context_filesync / context_derivation / context_compose /
                  context_merge / context_migrate / context_assetdb / context_import / context_editor_pkg
CLEAN context_editor_shell (12 targets in closure)
CLEAN context_editor       (13 targets in closure)
VERDICT: isolated
```

So the gate is **live and currently passing** — the forbidden targets exist in the build (not
vacuous) and the two audited closures are clean. The collision is **prospective**: hosting the
panels as-is would break it.

Per-panel link deps today:

| target | PUBLIC deps | verdict |
|---|---|---|
| `context_gui_panel_problems` | `context_gui_uitree`, `context_bridge` | ✅ clean — Shell-hostable |
| `context_gui_panel_scenetree` | `context_gui_uitree`, **`context_compose`**, `context_bridge` | ⛔ forbidden |
| `context_gui_panel_inspector` | `context_gui_uitree`, **`context_compose`**, **`context_schema`** | ⛔ forbidden |

**Cost is asymmetric — budget accordingly.**

- **scenetree is cheap.** One function:
  `build_scene_tree(const compose::ComposedScene&)`
  (`scenetree/include/.../scene_tree_model.h:59`, defined `src/scene_tree_model.cpp:145`).
  **11 call sites across 5 test files**: `tests/test_a11y.cpp`, `tests/test_scene_tree_model.cpp`
  (×5), `tests/test_scene_tree_panel.cpp` (×2), `src/tests/integration/test_m5exit1_walkthrough.cpp`,
  `src/tests/integration/test_m5exit3_seam_checklist.cpp` (×2).
  *(The ledger said "5 call sites" — that was the file count. The call-site count is 11.)*
- **inspector is a public-API change.** `build_inspector_model(const compose::ComposedEntity&,
  const schema::KindSchema&, const std::string& root_scene)` and
  `override_write_request(...) -> compose::WriteRequest`
  (`inspector/include/.../inspector_model.h:75,83`) both carry kernel types across the boundary.

⚠ **TWO CORRECTIONS to the ruling's own wording — trust this section, not the ledger prose:**

1. **`IWriteSink` does not exist** (zero hits repo-wide). The real seam is
   **`class OverrideWriteGateway`** (`inspector/include/.../inspector_panel.h:57`) with
   `virtual WriteAttempt attempt(const compose::WriteRequest&, std::uint64_t expected_raw_hash)` and
   `virtual FieldState read(root_scene, id_path, pointer)`. Its own doc comment records the
   implementors: **the CEF host** (over `compose::plan_write` + filesync atomic CAS) and **headless
   tests** (in-memory `plan_write`).
2. **The ruling's consumer list is incomplete** — the standing e05b lesson, confirmed here. Verified
   consumers of the inspector/scenetree model surfaces **outside their own directories**:
   `src/editor/gui/a11y/` · `src/editor/gui/session/` · `src/editor/gui/session/undo/`
   (`undo_journal.cpp`, `undo_test.h`) · **`src/editor/gui/viewport/`
   (`project_override_gateway.h` implements `inspector::WriteAttempt attempt(...)`,
   `viewport_edit_model.cpp`)** · `src/tests/integration/` (m5exit1 / m5exit2 / m5exit3) ·
   **`src/tests/concurrency/`**. The ledger named the CEF host, `context_gui_undo`, the a11y registry
   and the m5/m85 gates — it did **not** name **viewport** or **tests/concurrency**. Re-enumerate
   before you plan; this list is itself only a starting point.

## ⚠ Inherited from the e05d1 run (`8faaaee1fe17`) — verified, must be handled HERE

e05d1's retrospective surfaced findings it judged "best done with e05d3". They are folded into this
task's scope — do not defer them further:

- 🔴 **`uitree::render_html` mis-serializes the void `<input>` element.** For `textbox`/`checkbox`
  roles it emits `<input>…</input>` instead of a void `<input>`. Latent in e05d1 (no Shell-hostable
  panel used those roles) — **but the Inspector is built from exactly those roles**, so the broken
  serialization corrupts every Inspector field the moment this task hydrates it. Fix `render_html`'s
  void-element handling and cover it with an adversarial T1 string (the C-F6 escaping-contract lesson).
- **`float-cast-overflow`-shaped UB at `src/editor/shell/src/editor_state.cpp:41`** — a float→int
  cast that can overflow. Add the range guard (UBSan would trap it; same class as the e05b
  NaN→`int64` finding).
- **Perf, in the read/hydration path the Inspector stresses hardest**: one Problems click currently
  costs **3 full model builds (2 wasted)**, and the **hydration patcher is O(n²) in node count**.
  Address both while wiring live scenetree/inspector rather than deferring again.

## Goal

Refactor `context_gui_panel_scenetree` and `context_gui_panel_inspector` so the **panel libraries
are boundary-clean** (kernel-typed builders live elsewhere), then hydrate Scene tree + Inspector
from the live daemon through the e05d1 runtime — with `context_assert_shell_boundary` intact.

## Scope & seams

- **Shape**: keep model + panel boundary-clean; move the kernel-typed **builders** out of the panel
  libraries. The panel consumes a boundary-clean model type; whatever needs `compose::`/`schema::`
  types sits on the kernel side of the wire and reaches the panel as data.
- **The gate is the acceptance test.** A configure that still reports `VERDICT: isolated` with the
  FORBIDDEN list unchanged, `TARGETS context_editor_shell context_editor` unchanged, and the panel
  targets now in the audited closure, is the proof. Non-vacuity matters: the forbidden targets must
  still appear as `FORBIDDEN-PRESENT` in the build.
- **Do not regress the write path.** `OverrideWriteGateway` is the `context set` write path (L-35
  single source of truth) — the inspector must not grow a parallel write path. The CAS/L-30
  rebase-or-drop policy in `inspector_panel.cpp` stays behaviourally identical.
- **Hydration** reuses e05d1's panel-agnostic runtime. If e05d1 was built correctly, adding these
  two panels needs **no** hydration-runtime change — if you find yourself editing the runtime to
  special-case them, that is a signal e05d1 has a defect worth fixing rather than routing around.
- ♻ **Reusable asset**: worktree `.claude/worktrees/1eeb21321ae4` was preserved (`outcome=halted`)
  with a **completed** `cmake -S src --preset dev` and the boundary report on disk. Reuse it for the
  configure evidence rather than paying for a cold configure. **Do not destroy it until this task is
  done with it.**

## Standing lessons (carry forward — earned by the siblings)

1. **A spec's ripple list is a starting point, never the whole set** — already proven true *for this
   very task* (see correction 2 above). e05b's sixth consumer (`help::panel_topics()`) was caught
   locally, not by CI.
2. **Read CI before reviewing** (e05c) — on a NORMAL entry, not just a CI-failure re-entry.
3. **A passing sibling test only exonerates a suspected flake if that leg actually runs the affected
   code.** ⚠ This task changes link closures across a11y, undo, viewport and the m5 exit gates — the
   set of legs whose closure you touch is unusually wide here, so the "known flake" defence is
   correspondingly weak. Treat failures as REAL by default.
4. Known flakes: CE [#319](https://github.com/IvanMurzak/Context-Engine/issues/319) and CE
   [#322](https://github.com/IvanMurzak/Context-Engine/issues/322).
5. **Toolchain seam**: tool paths published by `src/runtime/ts` are NOT visible from `src/editor/`
   (configured first); `tsgo` is not even `PARENT_SCOPE`-exported.

## Definition of Done

- [ ] `context_gui_panel_scenetree` and `context_gui_panel_inspector` are **boundary-clean** — no
      `context_compose` / `context_schema` in their PUBLIC link interface
- [ ] `context_assert_shell_boundary`'s **FORBIDDEN list is byte-identical to e04's** and `TARGETS`
      still covers `context_editor_shell` + `context_editor` (a reviewer must verify this explicitly)
- [ ] Configure reports `VERDICT: isolated` **non-vacuously** (forbidden targets still
      `FORBIDDEN-PRESENT`; both closures `CLEAN`) with the panels now hostable
- [ ] Scene tree + Inspector hydrate from the **LIVE daemon** via the bridge (read path);
      interactions dispatch commands to the C++ models
- [ ] **No hydration-runtime special-casing** was needed for these two panels (or, if it was, the
      e05d1 defect that forced it is fixed rather than worked around)
- [ ] Every verified consumer still builds and passes: a11y, session, **undo**, **viewport**,
      **tests/concurrency**, and the **m5 + m85 exit gates**
- [ ] The `context set` write path is unchanged in behaviour; no parallel write path introduced
- [ ] `render_html` emits a **void** `<input>` for `textbox`/`checkbox` roles; an adversarial T1
      string covers it (inherited e05d1 blocker — the Inspector depends on it)
- [ ] `editor_state.cpp:41` float-cast UB fixed and UBSan-clean
- [ ] 3-OS CI green
