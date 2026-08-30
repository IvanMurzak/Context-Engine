# 04 — Panel instances, the manifest, and the Window menu

Covers tasks `c2`, `c3`, `d1`. Read before touching `extension.h`, `panel_host.h`, `panelhost.ts`,
`panels.ts`, layout restore, or `menu.ts`.

---

## 1. Why `dock.singleton` is decorative today

`PanelHost.#panels` is a `Map` keyed by manifest id (`panelhost.ts:808`), and `open` refuses on
`this.#panels.has(manifest.id)` (`:1091`) for **every** panel — the flag is never consulted. The C++
side is the same shape: `panel_host.h:168-254` keys `provide`/`render`/`invoke`/`gesture`/
`get_state`/`restore_state` by `panel_id`, and a provider binds **one** model, so a second instance
would have nothing to render from.

So "make some panels multi-instance" is not a feature flag. **Panel identity has to gain a second
component**, and every surface that names a panel has to carry it.

## 2. Manifest v3 (`c2`) — the declarative half

`kContractMajor` **2 → 3**. The compatibility window is exactly one major, so this refuses every v2
contribution the moment it lands. That is safe *today* — no out-of-repo consumers — and it is the
identical reasoning that made the 1 → 2 bump safe. The 1 → 2 bump's discipline is the precedent to
follow: **enumerate every in-repo consumer and move them in the same change**; each references the
constant symbolically rather than hardcoding a literal, which is what makes the enumeration tractable.

```jsonc
{
  "dock":       { "defaultZone": "right", "minWidth": 280, "minHeight": 200 },
  "instances":  { "mode": "singleton" | "limited" | "unlimited", "max": 4 },
  "path":       "Scene/Debug",
  "selection":  { "subjects": ["acme.tilemap.tile"] },
  "events":     { "publishes": ["acme.tilemap.brush"], "subscribes": ["other.pkg.thing"] }
}
```

- `dock.singleton` is **removed**, not deprecated — `instances.mode: "singleton"` replaces it exactly.
- `max` is meaningful only for `mode: "limited"`; the registry refuses `limited` without a positive
  `max`, and refuses `max` on the other two rather than silently ignoring it.
- `path` is slash-separated, empty means top level. The registry validates it as display text (no
  leading/trailing slash, no empty segment) — it is **not** a filesystem path and nothing resolves it.
- `selection.subjects` and `events.*` are validated for **namespacing under the declaring package id**,
  with the same discipline as `validatePackageTopic` (`uibus.ts`) and `validatePackageCommandId`
  (`panelverbs.ts:355`). A built-in may use an unnamespaced contract-owned name; a package may not.

Built-in roster consequences (`builtin_roster.cpp`): every entry's `singleton` argument becomes an
`instances` block, and every entry gains a `path`. `builtin.viewport` is the one already declared
non-singleton — it becomes `mode: "unlimited"` (multiple scene views is the point), and it is the
natural first proof that the instance runtime works.

## 3. The instance runtime (`c3`) — the imperative half

**Identity becomes `(panelId, instanceId)`.** `panelId` still names the *kind*; `instanceId` names the
live copy.

| Surface | Change |
|---|---|
| C++ `PanelHost` | The provider becomes a **factory**: one model per instance. Every method rekeys to the pair. `hostable_panel_ids()` still enumerates kinds |
| Wire | `instanceId` joins `panel.render`, `panel.command`, `panel.gesture`, `panel.state.get`, `panel.state.set` |
| `webui-panel-contract` | The byte-compare gate (`tools/check_webui_assets.py --panel-contract`) reads the vocabulary out of the **built bundle** — the TS and C++ constants move in the **same** change or the gate reds |
| `panels.ts` | `PanelManifest` carries `instances` + `path`; the parsers stay total and fail-closed exactly as they do now |
| `panelhost.ts` | `#panels` rekeys; `open` consults the mode; `openById` → `openInstance`; Dockview panel ids become instance ids |
| D6 state | Persisted per instance |
| Layout restore | `layoutrestore.test.ts` must round-trip instance ids |
| e10b tear-out / rehome | Moves an **instance**, not a kind |
| a11y | **Unchanged — keyed by kind** (planner ruling). Auditing N copies of one model proves nothing the first copy did not |

**Open semantics:**

- `singleton` — a second open **focuses** the existing instance and returns an honest "already open"
  outcome, not a failure.
- `limited` — refused past `max`, with a diagnostic naming the limit.
- `unlimited` — mints a new instance.

**Why `c2` and `c3` are two tasks:** `c2` is a declarative contract change with wide, shallow reach
(every consumer of the constant); `c3` is a deep runtime refactor through the panel host on both sides
of the wire. Merging them produces a PR nobody can review. `c3` depends on `c2`.

## 4. The Window menu (`d1`)

**D9: `Window` opens things; `Panel` acts on the focused one.**

`Window` becomes, in order: a search field · the panel tree built from `path` · a separator · the OS
window list that lives there today (`windowListEntries`, `menu.ts:205`). `Panel` (`menu.ts:290`) keeps
tear-out and the move actions unchanged.

**Search** matches every path segment **and** the panel name simultaneously — `dbg tile` finds
`Scene/Debug → Tilemap Painter`. Reuse `fuzzyMatch` from `palette.ts:102`; do not write a second
matcher. Its scoring already rewards consecutive runs, word starts and an early first match, which is
the ranking a path search wants, and it returns `{score, positions}` so matched characters can be
highlighted the way the palette highlights them.

**Instance rules are visible, not just enforced.** A singleton already open reads as "focus"; a
`limited` panel at its maximum renders **disabled with the reason in its tooltip** — the honest-degrade
rule `menu.ts` already applies to its ⏳ rows (`MENU_ITEM_DISABLED_REASON`, `CLIPBOARD_DISABLED_REASON`).
An item is disabled, never hidden, so the menu's shape cannot flicker.

**Constraints inherited from `menu.ts` that this must not break:**

1. **No second dispatch system.** Every entry names a command id in the one e07b registry. Opening a
   panel is a command (`view.panel.open.<panelId>`), registered like any other, so the palette and the
   keymap get it for free and the native macOS `NSMenu` path keeps working through `editor.ui.menu`.
2. **DOM only, no `innerHTML`** — every node built with `createElement` + `textContent`, since a
   package-authored panel title reaches this menu.
3. The dropdown is an app-chrome overlay in the palette's pattern, with ARIA `menubar`/`menu`/
   `menuitem` and full keyboard navigation. The **search field inside a menu is new** for this codebase:
   arrow keys must move through results while the field keeps text focus, and Escape must close the
   menu rather than only clearing the field.

`d1` depends on `c2` + `c3`. Building it first would mean rebuilding it.
