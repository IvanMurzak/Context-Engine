// The authored platformer-2d HUD (M7 T4 / a4, R-UI-001/006; M7 a12 exit content) — the TypeScript
// form of samples/platformer-2d/ui/hud.ui-hud.json. It builds the SAME headless UiTree the declarative
// document describes, through the engine-provided `context.ui` authoring surface that runs on the shipped
// V8 host: a column HUD panel with a score label + a health bar (each read-only data-bound to numeric
// state) and a collect button whose onClick scores points and spends health (the UI->state action path —
// the platformer's coin-collect loop).
//
// UI is presentation (D6): the tree lives OUTSIDE the sim World and its state store folds into no state
// hash, so building/updating this HUD never perturbs the deterministic simulation (the m7-exit-4
// determinism-presentation gate). The `context.ui` surface is doubles-only across the JsEngine host seam,
// so the closed Role/EventType/Positioning/Flow vocabularies + node handles + integer state keys all
// marshal as numbers — mirrored 1:1 by the C++ UiScriptContext (src/packages/ui/script_bindings.h).
// Authored in the same discipline as the a4 headline example src/runtime/ts/examples/ui_hud.ts.

/** App-defined numeric state keys (doubles-only protocol); match the `key` fields in hud.ui-hud.json. */
const HudState = {
    Score: 100,
    Health: 200,
} as const;

/** Build the platformer HUD under the retained tree's root. Idempotent enough to re-run. */
function buildHud(): void {
    const ui = context.ui;
    const root = ui.root();

    // A column HUD panel: CSS-like style props (semi-opaque dark background + padding).
    const panel = ui.panel(root)
        .layout({ position: Positioning.Flow, w: 220, h: 96, flow: Flow.Column, gap: 6 })
        .background(0, 0, 0, 160)
        .padding(8);

    // Score label — read-only data binding to the score state (starts at 0).
    ui.writeState(HudState.Score, 0);
    ui.label(panel).foreground(255, 214, 0).bindValue(HudState.Score);

    // Health bar — read-only data binding to the health state (starts at 100).
    ui.writeState(HudState.Health, 100);
    ui.progressBar(panel).foreground(0, 200, 0).bindValue(HudState.Health);

    // Collect button — clicking it scores 10 points and costs 5 health (UI -> state action path).
    ui.button(panel)
        .background(40, 40, 60)
        .onClick((_ev) => {
            ui.addState(HudState.Score, 10);
            ui.addState(HudState.Health, -5);
        });
}

buildHud();

// --- ambient `context.ui` authoring surface (installed on the V8 host by src/runtime/ts/) -------------
// The doubles-only builder the engine binds as a global; declared here so this authored sample is
// self-contained (the movement.ts `declare const ctx` discipline). Mirrors src/packages/ui/script_bindings.h.
declare const enum Positioning { Flow = 0, Absolute = 1 }
declare const enum Flow { None = 0, Row = 1, Column = 2 }
interface UiNodeBuilder {
    readonly id: number;
    layout(box: { position?: Positioning; w?: number; h?: number; flow?: Flow; gap?: number }): this;
    background(r: number, g: number, b: number, a?: number): this;
    foreground(r: number, g: number, b: number, a?: number): this;
    padding(px: number): this;
    bindValue(key: number): this;
    onClick(fn: (ev: unknown) => void): this;
}
declare const context: {
    ui: {
        root(): UiNodeBuilder;
        panel(parent: UiNodeBuilder): UiNodeBuilder;
        label(parent: UiNodeBuilder): UiNodeBuilder;
        progressBar(parent: UiNodeBuilder): UiNodeBuilder;
        button(parent: UiNodeBuilder): UiNodeBuilder;
        writeState(key: number, value: number): void;
        addState(key: number, delta: number): number;
    };
};
