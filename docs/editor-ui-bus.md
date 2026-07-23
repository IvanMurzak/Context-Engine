# The `editor.ui` bus (M9 e08c)

The editor-local event bus for **UI chrome facts** — D7's second tier. Design authority: 05 §5 / 02
(outside this repo); implementation: `src/editor/webui/core/src/uibus.ts`.

## The two tiers, and why chrome is not one of them

D7 splits the editor's facts in two:

| tier | facts | lives | reaches |
|---|---|---|---|
| 1 — semantic | selection, cameras, play state | the **daemon** (`docs/editor-session-state.md`, e08a) | every client: CLI, agents, other windows |
| 2 — chrome | focus, layout, drag, viewport hover, theme, palette | **editor-core**, this bus | this editor's windows, and nothing else |

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
* **`topic`** — the built-in six are closed: `editor.ui.focus`, `.layout`, `.drag`, `.viewport`,
  `.theme-changed`, `.palette`. A package may publish only topics it **declared in its manifest**,
  namespaced under its own package id (`acme.tilemap.brush-changed`); anything else is refused with a
  diagnostic. The `ui_events` **capability** — whether the package may ride the bus at all — is
  enforced end to end by **e13**; this module owns the declaration + namespacing half only.
* **`origin`** — the window that published the fact. What makes a mirrored envelope distinguishable
  from a local one, and therefore what makes echo suppression possible.
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
  local subscriber) with its `origin` preserved, and is **not re-delivered to that bus's own mirror
  sinks**. That is the echo suppression: without it two cross-attached windows ring forever. Both
  properties are asserted by the two-bus drill in `uibus.test.ts`.
* Nothing wires a sink at boot, **deliberately**. The Shell's bridge router denies unknown methods, so
  a boot-time call to a not-yet-existing mirror method would redden every CEF smoke that had not
  installed a stub for it.

A sink implementation MUST target a **Shell-local** method. The Shell mirrors chrome between its own
windows; routing chrome onward to the daemon is the D7 violation the gates above exist to prevent.
(No daemon-forwarding bridge method exists today — every method the Shell routes is Shell-local — so
when one is added, `check_ui_bus_boundary.py` should grow a deny-list naming it.)

## Publishers today

`ThemeEngine.apply` (e06b, `theme.ts`) publishes `editor.ui.theme-changed`. e06b shipped a local
*stub* bus so it could publish before this one existed; e08c **deleted** that stub rather than leave a
placeholder to fossilize, and no consumer changed: `ThemeEngineOptions.bus`, `ThemeEngine.bus` and
`IframeThemeChannel` all kept their shapes, and the iframe channel is now simply the bus's first
subscriber instead of a second hand-rolled fan-out.

The remaining five topics have no publisher yet — they are the vocabulary the focus / layout / drag /
viewport / palette surfaces publish onto as each is wired (the when-context sources in `when.ts` are
their natural first consumer).
