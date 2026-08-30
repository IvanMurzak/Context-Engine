---
id: "f1-uibus-boundary-denylist"
title: "The owed editor.ui boundary deny-list: panel.daemon.call + d2's publish verb, verified by a planted violation"
group: "F"
sequence: 1
repo: "."
base_branch: "main"
depends_on: ["d2-package-fact-bus"]
importance: 8
complexity: 7
security_critical: true
production_touching: false
model_hint: "top"
taskflow_refs: ["01-current-architecture.md", "05-selection-and-package-events.md"]
---

## Goal

`docs/editor-ui-bus.md` records this debt in its own words: `panel.daemon.call` now exists, the
premise that a mirror sink *could not* reach the daemon is gone, `check_ui_bus_boundary.py` owes it a
deny-list entry, and *"this is not yet implemented — so today the rule is enforced by review rather
than by the gate."* `d2` added a **second** daemon-reaching method (the package publish verb), which
widens exactly that hole. Close the debt: the D7 boundary ("chrome never reaches the daemon") becomes
gate-enforced for both methods.

## Scope & seams

- **`tools/check_ui_bus_boundary.py`**: add the deny-list naming both daemon-reaching methods —
  `panel.daemon.call` and the `d2` publish verb — forbidden from the `editor.ui` bus's
  mirror/forwarding surfaces, in the checker's existing style.
- The ctest is **`webui-uibus-boundary`**; `uibus.test.ts` holds the TS-side half of the D7 boundary
  and is extended where the deny-list's behaviour is observable there.
- **`docs/editor-ui-bus.md`** updated: the "enforced by review rather than by the gate" sentence is
  replaced by a statement of what the gate now enforces, and the owed-entry paragraph is resolved.
- Out of scope: any change to the bus's nine built-in topics (frozen); any change to
  `panel.daemon.call` or the publish verb themselves; new checker infrastructure beyond the entry.

## Definition of Done

- **Verified by planting a forwarding path** (the set's named gate — the discipline the original
  checker was built with): introduce a violation (a mirror sink forwarding a bus fact into
  `panel.daemon.call`, and separately into the publish verb) and **watch the gate go RED**; revert
  the plant and watch it go green. The PR body records both plants' red output — a boundary test
  that would still pass with a violation in place is worse than none.
- Both methods are denied by the checker; the existing boundary assertions all still pass (the
  deny-list narrows, never loosens).
- A compliant use (a panel legitimately calling `panel.daemon.call` from its own panel surface, not
  from a bus mirror) still passes — the sibling proving the gate discriminates rather than blanket-
  fails.
- `webui-uibus-boundary` green; `docs/editor-ui-bus.md` no longer records the debt.
- PR body cites D4/D7 and links the two plant runs.
