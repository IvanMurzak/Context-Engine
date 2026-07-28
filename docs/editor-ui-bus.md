# The `editor.ui` bus (M9 e08c)

The editor-local event bus for **UI chrome facts** — D7's second tier. Design authority: 05 §5 / 02
(outside this repo); implementation: `src/editor/webui/core/src/uibus.ts`.

## The two tiers, and why chrome is not one of them

D7 splits the editor's facts in two:

| tier | facts | lives | reaches |
|---|---|---|---|
| 1 — semantic | selection, cameras, play state | the **daemon** (`docs/editor-session-state.md`, e08a) | every client: CLI, agents, other windows |
| 2 — chrome | focus, layout, drag, viewport hover, theme, palette, refused-write notices | **editor-core**, this bus | this editor's windows, and nothing else |

Selection and camera are answers to "what is the human working on" — an agent needs them, so they are
daemon state. "Which tab has focus" is not: it is noise to a daemon whose subject is authored data,
and forwarding it would put per-frame chrome traffic on the wire that carries project truth.

## The envelope

Deliberately the daemon stream's (`src/editor/bridge/include/context/editor/bridge/event_stream.h`),
so a consumer written against one reads the other:

```ts
{ seq: number, topic: string, origin: string, payload: unknown }
```

* **`seq`** — monotonic and totally ordered *within one bus* (one window), exactly as the daemon's is
  within one incarnation. A refused publish consumes none.
* **`topic`** — the built-in **seven** are closed: `editor.ui.focus`, `.layout`, `.drag`,
  `.viewport`, `.theme-changed`, `.palette`, and — since M9 e09b-3 — `.write-notice` (a write the
  editor REFUSED: an L-30 concurrent-writer drop, or a write-path refusal; and, since M9 x10, an
  Inspector gesture the editor ABANDONED, where no write was attempted at all). Design 05 §5 enumerates
  the first six; the seventh is required by §8's canonical write flow, which ends *"drop LOUDLY +
  notification + `editor.ui` fact"*, and by design 10's non-negotiable *"Destructive/lossy moments …
  are LOUD (wait/bad hues), never silent"*. The set is still CLOSED — adding a member is a
  deliberate act with an authority, not a door left open. A package may publish only topics it
  **declared in its manifest**,
  namespaced under its own package id (`acme.tilemap.brush-changed`); anything else is refused with a
  diagnostic. The `ui_events` **capability** — whether the package may ride the bus at all — is
  enforced end to end by **e13**; this module owns the declaration + namespacing half only.
* **`origin`** — the window that published the fact — *or* the literal `"shell"` on
  `editor.ui.write-notice`, the one topic published by the C++ Shell rather than by a window (see
  § Publishers today for why that distinction is load-bearing rather than cosmetic). What makes a
  mirrored envelope distinguishable from a local one, and therefore what makes echo suppression
  possible.
* **Snapshot-on-subscribe** — a subscriber is handed the retained envelope for its topic immediately.
  A panel mounted after a theme switch must not render untokened, and "subscribe, then separately ask
  for current state" is the race that model exists to remove.

**Facts only, never commands.** A topic reports what happened; nothing on this bus dispatches. There
is no request/response shape here by construction, which is also why publishing is total and
fire-and-forget.

## The D7 boundary is asserted, not documented

"Chrome facts never reach the daemon" is a hard boundary, so it is held down mechanically by two
checks that catch different things. Both were verified by **planting a forwarding path** and watching
them go red — a boundary test that would still pass with a violation in place is worse than none.

1. **`webui-uibus-boundary`** (ctest; `tools/check_ui_bus_boundary.py`) — a source scan on every
   `build` leg. `uibus.ts` may not name *or invoke* editor-core's one exit (`ShellBridge`,
   `bridge.ts`), and no `editor.ui.*` subscription anywhere in editor-core may reach that exit from
   its callback. This is the half that sees a forwarding path **no test happens to drive**.
2. **`webui-ts-unit` → `uibus.test.ts`** — installs a recording query function as the injected Shell
   channel (the one channel out of the renderer), drives the real theme engine plus a publish on every
   topic with subscribers and a mirror sink attached, and requires the channel to have seen nothing.
   This is the half that sees a forwarding path that **compiles past the source scan**.

## Cross-window mirroring: a SEAM here, a drill in e10

The design has this bus "mirrored across windows via the Shell". e08c builds and unit-tests the
**seam**; the multi-window propagation **drill** is e10's, which owns multi-window (TD ruling
2026-07-22).

What exists today:

* `UiMirrorSink { deliver(event) }` — attach one with `bus.attachMirror(sink)`. It receives every
  locally-published envelope, plus the current snapshot set on attach (a window joining late must not
  have to guess the chrome state it missed).
* `bus.receiveMirrored(event)` — the receiving end. The envelope is **re-sealed with the receiving
  bus's own `seq`** (a foreign counter spliced into a local one would break monotonicity for every
  local subscriber) with its `origin` preserved. Two INDEPENDENT loop breakers then keep the seam
  echo-free, and a transport needs whichever one matches its shape: a mirrored envelope is **not
  re-delivered to that bus's own mirror sinks** (this terminates a ring of point-to-point sinks), and
  an envelope arriving back at its **own `origin` is dropped** (this is what saves a transport that
  BROADCASTS to every window including the sender — the shape a Shell hop most naturally takes). Each
  has its own case in `uibus.test.ts`; the two-bus ring drill exercises only the first.
* Nothing wires a sink at boot, **deliberately**. The Shell's bridge router denies unknown methods, so
  a boot-time call to a not-yet-existing mirror method would redden every CEF smoke that had not
  installed a stub for it.

A sink implementation MUST target a **Shell-local** method. The Shell mirrors chrome between its own
windows; routing chrome onward to the daemon is the D7 violation the gates above exist to prevent.

⚠ **A daemon-forwarding method now EXISTS** — `panel.daemon.call` (M9 e13c-1), which carries a package
panel's call onto that package's own baseline daemon session. Until e13b-1 this section could argue
that a sink *could not* reach the daemon because no routed method reached it through; that premise is
gone. `check_ui_bus_boundary.py` accordingly owes the deny-list naming it, which is **not yet
implemented** — so today the rule "a sink MUST target a Shell-local method" is enforced by review
rather than by the gate. Pointing a sink at `panel.daemon.call` would put chrome facts on the daemon
wire.

## Publishers today

`ThemeEngine.apply` (e06b, `theme.ts`) publishes `editor.ui.theme-changed`. e06b shipped a local
*stub* bus so it could publish before this one existed; e08c **deleted** that stub rather than leave a
placeholder to fossilize, and no consumer changed: `ThemeEngineOptions.bus`, `ThemeEngine.bus` and
`IframeThemeChannel` all kept their shapes, and the iframe channel is now simply the bus's first
subscriber instead of a second hand-rolled fan-out.

`editor.ui.write-notice` (e09b-3) is the **only topic whose publisher is the C++ Shell**, and the
shape is worth knowing because it is the editor's one Shell-to-renderer push path:

* the Shell's `WriteNoticeRelay` (`src/editor/shell/include/context/editor/shell/write_notice.h`)
  turns a refused write into a `{seq, topic, origin: "shell", payload}` envelope and queues it for
  **every live window** through the SAME `UiMirrorStore` the cross-window mirror uses. No second poll
  surface was invented: the e05c bridge accepts no persistent queries, so `ui.mirror-poll` is the one
  channel available, and re-using it also meant the nine live `editor-cef-smoke-shell*` scenarios
  needed no edit (a new boot-time method would be an `unknown_method` refusal in each);
* the `origin` is `"shell"`, **never a window id** — `receiveMirrored` drops an envelope whose origin
  equals the receiving bus's own, and every bus origin *is* a window id, so a window-stamped notice
  would be swallowed by exactly the window it was meant for;
* editor-core's `UiMirrorPoller` applies it to the bus like any mirrored fact, and
  `createNotificationHost` (`notifications.ts`) renders it: `wait` hue for a drop (06 §2's
  awaiting-human — nothing was lost, re-apply against the current value), `bad` for a refusal
  (nothing was written), and — since M9 x10 (CE #452) — `wait` again for an **abandoned** gesture, with
  its own sentence: no write was attempted at all, the Inspector had to load a different model while an
  edit was in flight, so the human is told the SELECTION changed and to re-select that entity. Sharing
  the drop's hue is right (the human is who must act); sharing its sentence would not be — there was no
  compare-and-swap and no co-writer need exist. Always in the kit's **assertive** live region so colour
  is never the only signal. The topic string and all THREE kind tokens are byte-compared against
  `write_notice.h` by `tools/check_webui_assets.py --panel-contract`;
* the **payload** is `{kind, action, code, message, pointer}`, every member always present (an
  absent-vs-empty distinction would mean nothing here, so the Shell writes all five unconditionally
  and `parseWriteNotice` stays total). `kind` is one of the three pinned tokens; `action` is free text
  the Shell composes (`"edit"` / `"undo"` / `"redo"`) and the renderer only displays; `code` is the
  catalog code the write path answered with — or, for an abandoned gesture, the host-minted
  `shell.gesture_abandoned`, deliberately NOT a catalog entry because no daemon verb was called;
  `pointer` is the field the refused (or abandoned) write targeted, empty
  when there was none. Note the MEMBER NAMES themselves are not pinned across the boundary — only the
  topic and the three kinds are — so a rename on one side degrades through the renderer's total parser
  rather than failing a gate.

The remaining five topics have no publisher yet — they are the vocabulary the focus / layout / drag /
viewport / palette surfaces publish onto as each is wired (the when-context sources in `when.ts` are
their natural first consumer).
