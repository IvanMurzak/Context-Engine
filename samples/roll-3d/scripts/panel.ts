// The authored roll-3d world-space panel (M7 T4 / a4, R-UI-001/006; M7 a12 exit content) — the
// TypeScript form of samples/roll-3d/ui/panel.ui-worldpanel.json. It builds the SAME headless UiTree
// the declarative `panel` block describes, through the engine-provided `context.ui` authoring surface on
// the shipped V8 host: a diegetic arena info panel (a status label read-only data-bound to state + an
// action button). The panel content is authored exactly like a screen-space HUD; what makes it
// world-space is the render-side placement of its dynamic-texture surface on a curved (cylinder-section)
// mesh (a9 flat -> a10 curved, the `world` block of the declarative document) — the authoring surface is
// backend-agnostic (D1).
//
// UI is presentation (D6): the tree lives OUTSIDE the sim World, so building/updating this panel never
// perturbs the deterministic simulation (m7-exit-4), and a pointer landing on it routes through the SAME
// a3 single sink a keybind uses (m7-exit-3). The `context.ui` surface is doubles-only across the JsEngine
// host seam. Authored in the discipline of the a4 headline example src/runtime/ts/examples/ui_hud.ts.

/** App-defined numeric state key (doubles-only protocol); matches the `key` in panel.ui-worldpanel.json. */
const PanelState = {
    Activated: 300,
} as const;

/** Build the roll-3d world-space panel under the retained tree's root. Idempotent enough to re-run. */
function buildPanel(): void {
    const ui = context.ui;
    const root = ui.root();

    // The panel surface (sized to the dynamic texture the world mesh samples): a dark, semi-opaque board.
    const panel = ui.panel(root)
        .bounds(0, 0, 200, 120)
        .background(10, 12, 24, 200)
        .padding(8);

    // Status label — read-only data binding to the `activated` state (0 until the panel is clicked).
    ui.writeState(PanelState.Activated, 0);
    ui.label(panel).bounds(12, 10, 176, 24).foreground(200, 220, 255).bindValue(PanelState.Activated);

    // Action button covering the panel centre — a pointer landing here (via the raycast->UV->coords
    // chain) sets `activated` (the UI->state action path); the same intent a keybind would emit.
    ui.button(panel)
        .bounds(70, 40, 60, 40)
        .background(40, 40, 60)
        .onClick((_ev) => {
            ui.setState(PanelState.Activated, 1);
        });
}

buildPanel();

// --- ambient `context.ui` authoring surface (installed on the V8 host by src/runtime/ts/) -------------
// Declared here so this authored sample is self-contained (the movement.ts `declare const ctx`
// discipline). Mirrors src/packages/ui/script_bindings.h.
interface WorldPanelBuilder {
    readonly id: number;
    bounds(x: number, y: number, w: number, h: number): this;
    background(r: number, g: number, b: number, a?: number): this;
    foreground(r: number, g: number, b: number, a?: number): this;
    padding(px: number): this;
    bindValue(key: number): this;
    onClick(fn: (ev: unknown) => void): this;
}
declare const context: {
    ui: {
        root(): WorldPanelBuilder;
        panel(parent: WorldPanelBuilder): WorldPanelBuilder;
        label(parent: WorldPanelBuilder): WorldPanelBuilder;
        button(parent: WorldPanelBuilder): WorldPanelBuilder;
        writeState(key: number, value: number): void;
        setState(key: number, value: number): void;
    };
};
