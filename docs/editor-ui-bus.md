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
* **`topic`** — the built-in **nine** are closed: `editor.ui.focus`, `.layout`, `.drag`,
  `.viewport`, `.theme-changed`, `.palette`, — since M9 e09b-3 — `.write-notice` (a write the
  editor REFUSED: an L-30 concurrent-writer drop, or a write-path refusal; and, since M9 x10, an
  Inspector gesture the editor ABANDONED, where no write was attempted at all), — since
  editor-window-chrome a1 — `.chrome` (a window's maximized state flipped, observed by the Shell's
  placement poll; payload `{windowId, maximized}`; the titlebar's max/restore glyph is its
  consumer), and — since editor-window-chrome d3 — `.menu` (a native NSMenu item was activated;
  payload `{windowId, commandId}`; editor-core executes the id through the ONE e07b registry, so
  the native menu never grows a second dispatch system). Design 05 §5 enumerates
  the first six; the seventh is required by §8's canonical write flow, which ends *"drop LOUDLY +
  notification + `editor.ui` fact"*, and by design 10's non-negotiable *"Destructive/lossy moments …
  are LOUD (wait/bad hues), never silent"*; the eighth by the window-chrome target design's 02 §1
  (*"changes are broadcast as a fact on the existing `editor.ui` mirror relay"*); the ninth by the
  menu structure design 03 (*"an activated item comes back as a fact on the EXISTING `editor.ui`
  relay carrying the command id"*). The set is still CLOSED — adding a member is a
  deliberate act with an authority, not a door left open. A package may publish only topics it
  **declared in its manifest**,
  namespaced under its own package id (`acme.tilemap.brush-changed`); anything else is refused with a
  diagnostic. The `ui_events` **capability** — whether the package may ride the bus at all — is
  enforced end to end by **e13**; this module owns the declaration + namespacing half only.
* **`origin`** — the window that published the fact — *or* the literal `"shell"` on
  `editor.ui.write-notice`, `editor.ui.chrome` and `editor.ui.menu`, the three topics published by
  the C++ Shell rather than by a window (see
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
   `build` leg, in three rules. `uibus.ts` may not name *or invoke* editor-core's one exit
   (`ShellBridge`, `bridge.ts`); no `editor.ui.*` subscription anywhere in editor-core may reach that
   exit from its callback; and — since editor-UX `f1` — no **mirror-bearing** module may name a
   **daemon-reaching** method (the deny-list, below). This is the half that sees a forwarding path
   **no test happens to drive**.
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

**That rule is now GATE-ENFORCED, not review-enforced** (editor-UX `f1`). Two methods the Shell routes
do reach the daemon, and either one, named from a mirror sink, would put this window's chrome facts on
the daemon wire:

* `panel.daemon.call` (M9 e13c-1, `package_sessions.h` `kPanelDaemonCallMethod`) — a package panel's
  call, forwarded onto that package's own baseline daemon session;
* `panel.facts.publish` (editor-UX `d2`, `package_facts.h` `kPanelFactsPublishMethod`) — the package
  FACT BUS's one door ([`package-facts.md`](package-facts.md)), carrying a package's declared fact
  onto that same session.

Until `f1` the safety argument here rested on a premise that had expired: that a sink *could not*
reach the daemon because every routed method was Shell-local. `check_ui_bus_boundary.py` rule 3 now
carries it instead. A module is **mirror-bearing** when it implements `UiMirrorSink` or calls
`attachMirror(`, and a mirror-bearing module may not name either method anywhere in it — by wire
literal or by the editor-core constant that mirrors it (`PANEL_DAEMON_CALL_METHOD`,
`PANEL_FACTS_PUBLISH_METHOD`).

**Whole-module, and that is the design, not laziness.** A rule scoped to the sink's class body or the
`attachMirror` argument is walked past by a file-local helper, a file-local
`const M = "panel.daemon.call"`, or a renaming import — each needing another regex that has to be
right. Holding the whole module clean is the same structural-incapability argument rule 1 makes about
`uibus.ts`, one module out, and it costs the seam nothing: `uibus.ts` and `uimirror.ts` are the only
mirror-bearing modules and neither has any business naming a daemon verb.

**It discriminates, rather than banning the methods outright.** A *compliant* use of either —
`makePackageDaemonCall` (`boot.ts`) and `makePackageFactPublish` (`packagefacts.ts`), each forwarding a
panel's own call from a panel surface — lives in a module that bears no sink and stays green. `boot.ts`
is the sharp case: it both defines `PANEL_DAEMON_CALL_METHOD` **and** brings this mirror up (via
`wireUiMirror`), and it passes, because bringing a transport up is not holding a sink. Attach one
there and it fails.

Verified the way this checker was verified originally — by planting a forwarding path and watching the
gate go red, in both directions and per plant: `panel.daemon.call` from `ShellUiMirrorSink.deliver`,
`panel.facts.publish` from the same place, and the helper-plus-local-constant indirection a
region-scoped rule would have missed. `uibus.test.ts` carries the runtime half: it drives the REAL
`ShellUiMirrorSink` and asserts the methods it puts on the wire are Shell-local `ui.mirror*` verbs —
after first asserting the channel carried anything at all, because "no daemon verb was called" is free
for a seam that called nothing.

**What the deny-list does NOT cover, named rather than left implied.** It is a list of *names*, so a
third daemon-reaching router method added later is unguarded until it is added to the list, and a sink
reaching one through a constant imported from another module under a fresh name is not matched by
name. And the scope is a *module*, so — the one to state plainly, because "a mirror sink can no longer
reach the daemon" would otherwise be read as absolute — **the sink object can be built one module
out**. A factory returning `{ deliver: (e) => void bridge.call(PANEL_FACTS_PUBLISH_METHOD, e) }` names
no sink type and attaches nothing, so its module is not mirror-bearing; the module that then calls
`attachMirror(factory(bridge))` is mirror-bearing but names no denied method. Both halves sweep clean
with a live forwarding path in the tree — measured against the checker, not hypothesised, and pinned
by `test_the_cross_module_sink_factory_is_a_KNOWN_residual` so the gap stays a measurement rather than
an assumption. Whole-module scope beats the *file*-local helper it was chosen for; it does not beat a
cross-module one, and no name-based rule can. What stands behind it is rule 2, the runtime half, and a
review duty: **a new mirror sink belongs in `uimirror.ts`**, which is where rule 3 reaches it.
`webui-panel-contract` byte-compares both entries against the C++ headers and the built bundle,
and `test_check_ui_bus_boundary.py` re-reads those headers to assert each denied literal still exists
there, so a **rename** cannot silently empty the list — but an **addition** is a human duty. Nor does
this gate touch the security findings `d2` shipped open (both its daemon verbs sit on the read/query
baseline; the manifest topic grammar has no length bound while the bus enforces 128); those are daemon-
and manifest-tier issues on the far side of this boundary, and the deny-list neither closes nor
worsens them.

The `editor.ui` built-in topic set is **unchanged at nine** by `d2`, deliberately: package facts are
daemon facts precisely so this bus does not have to grow a member, and `packageui.ts`'s refusal of
cross-package `editor.ui` subscription **stays** — D4 answers that refusal's security reason with an
explicit, operator-consented grant on the daemon tier rather than by removing the check.

## Publishers today

`ThemeEngine.apply` (e06b, `theme.ts`) publishes `editor.ui.theme-changed`. e06b shipped a local
*stub* bus so it could publish before this one existed; e08c **deleted** that stub rather than leave a
placeholder to fossilize, and no consumer changed: `ThemeEngineOptions.bus`, `ThemeEngine.bus` and
`IframeThemeChannel` all kept their shapes, and the iframe channel is now simply the bus's first
subscriber instead of a second hand-rolled fan-out.

`editor.ui.chrome` (editor-window-chrome a1) is Shell-published too, on the same relay with the same
`"shell"` origin, but **unicast**: the Shell's `ChromeFactRelay` (`chrome_facts.h`) queues a
`{windowId, maximized}` flip only for the window whose chrome changed — a maximized glyph is that
window's own fact, where a refused write below is app-wide news.

`editor.ui.menu` (editor-window-chrome d3) is the chrome fact's exact sibling: the Shell's
`MenuActivationRelay` (`menu_facts.h`) queues a `{windowId, commandId}` envelope — a native NSMenu
item was activated (a click on the macOS menu bar, or its key equivalent) — **unicast** to the
window whose menu it is (the primary owns the app menu bar today). menu.ts's `subscribeMenuFacts`
executes the command id through the ONE e07b registry, which is what keeps 03's "no second dispatch
system" true across the native rendering.

`editor.ui.write-notice` (e09b-3) was the **first topic whose publisher is the C++ Shell**, and the
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
  its own sentence: no write was attempted at all, the Inspector had to replace the model under an
  in-flight edit, so the human is told their edit was discarded and to re-open the field and re-apply
  it. That sentence names no CAUSE, deliberately: a notice renders from `kind` alone, and an
  abandonment is produced BOTH by a foreign selection move AND by a same-entity re-read (the
  read-your-writes path — see `inspector_feed.cpp`), so asserting "the selection changed" would be
  false on the commoner of the two. The specific cause travels in `message`, which the Shell composes
  where both identities are known. Sharing the drop's hue is right (the human is who must act); sharing
  its sentence would not be — there was no compare-and-swap and no co-writer need exist. Always in the
  kit's **assertive** live region so colour is never the only signal. The topic string and all THREE
  kind tokens are byte-compared against `write_notice.h` by
  `tools/check_webui_assets.py --panel-contract`;
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
