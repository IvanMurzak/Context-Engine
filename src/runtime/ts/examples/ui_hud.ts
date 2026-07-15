// An authored in-game HUD (M7 T4 / a4, R-UI-001/006) — the headline proof that the `context.ui` TS
// authoring surface runs on the shipped V8 host and builds a real headless UiTree. Bundled with esbuild
// `--bundle --format=iife`, so the top-level buildHud() call runs when the runtime/js host evaluates the
// emitted JS: the tree is populated, a score label + health bar are bound (read-only) to numeric state,
// and a button carries an onClick that scores points (the UI->state action path). A headless driver
// then dispatches a pointer-down on the button and asserts the state readback — no GPU, no renderer.
//
// Doubles-only host seam: node handles cannot be returned across it as objects, so the interactive node
// ids are stashed into state keys the driver reads back.

import { ui, EventType, Positioning, Flow } from "./context_ui";

// App-defined numeric state keys (the doubles-only protocol keys state by number).
export const HudState = {
    Score: 100,
    Health: 200,
    ButtonNode: 900, // stash: the play-button node id, for a headless driver to target
    ScoreNode: 901,  // stash: the score-label node id, to assert its bound value
} as const;

// Build the HUD under the root. Idempotent enough to re-run; exposed as globalThis.buildHud() too so a
// re-entrant driver can rebuild.
export function buildHud(): void {
    const root = ui.root();

    // A column HUD panel: CSS-like style props (semi-opaque dark background + padding).
    const panel = ui.panel(root)
        .layout({ position: Positioning.Flow, w: 220, h: 96, flow: Flow.Column, gap: 6 })
        .background(0, 0, 0, 160)
        .padding(8);

    // Score label — read-only data binding to the score state (starts at 0).
    ui.writeState(HudState.Score, 0);
    const score = ui.label(panel)
        .foreground(255, 214, 0)
        .bindValue(HudState.Score);

    // Health bar — read-only data binding to the health state (starts at 100).
    ui.writeState(HudState.Health, 100);
    ui.progressBar(panel)
        .foreground(0, 200, 0)
        .bindValue(HudState.Health);

    // Play button — clicking it scores 10 points and costs 5 health (UI -> state action path).
    const button = ui.button(panel)
        .background(40, 40, 60)
        .onClick((_ev) => {
            ui.addState(HudState.Score, 10);
            ui.addState(HudState.Health, -5);
        });

    // Stash the interactive node ids so a headless driver can target them (doubles-only: ids travel
    // through state, not object returns).
    ui.writeState(HudState.ButtonNode, button.id);
    ui.writeState(HudState.ScoreNode, score.id);
}

buildHud();
(globalThis as any).buildHud = buildHud;
