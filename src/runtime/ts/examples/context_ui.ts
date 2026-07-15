// The `context.ui` runtime-UI authoring surface (M7 T4 / a4, R-UI-001, owner ruling (a)): a TS
// RETAINED-TREE API with CSS-like style props — NOT an HTML/CSS parser (file-level fidelity arrives
// later with the optional CEF backend). Authored TS runs on the shipped V8 host (src/runtime/js/) and
// drives a headless context_ui UiTree through a set of doubles-only host primitives the engine binds as
// globals (script_bindings.h): tree construction, style props, event handlers, and read-only data
// binding to numeric state queries. Only doubles cross the host seam, so roles / event kinds / node
// handles / state keys are all numbers — the enum values below MIRROR the C++ enum declaration order.
//
// This module also installs the `__ui_invoke` callback the host calls back into on event dispatch (the
// C++ tree handler -> JsEngine::callFunction("__ui_invoke", ...) bridge), so authored onClick handlers
// run in-VM. Imported by an authored HUD (ui_hud.ts) and bundled with esbuild `--bundle --format=iife`.

// --- doubles-only host primitives (bound by the host before eval) --------------------------------
declare function __ui_create(parent: number, role: number): number;
declare function __ui_set_opacity(node: number, opacity: number): number;
declare function __ui_set_visible(node: number, visible: number): number;
declare function __ui_set_background(node: number, r: number, g: number, b: number, a: number): number;
declare function __ui_set_foreground(node: number, r: number, g: number, b: number, a: number): number;
declare function __ui_set_padding(node: number, padding: number): number;
declare function __ui_set_layout(
    node: number, position: number, w: number, h: number, flow: number, gap: number): number;
declare function __ui_set_bounds(node: number, x: number, y: number, w: number, h: number): number;
declare function __ui_on(node: number, eventType: number, handlerId: number): number;
declare function __ui_bind_value(node: number, stateKey: number): number;
declare function __ui_write_state(key: number, value: number): number;
declare function __ui_add_state(key: number, delta: number): number;
declare function __ui_read_state(key: number): number;

// --- the closed vocabularies (MUST match the C++ enum declaration order in ui_node.h/events.h) ----
export const Role = {
    Root: 0, Panel: 1, Group: 2, Label: 3, Button: 4, Image: 5,
    Slider: 6, Checkbox: 7, TextInput: 8, ProgressBar: 9, List: 10, ListItem: 11,
} as const;

export const EventType = {
    PointerDown: 0, PointerUp: 1, PointerMove: 2, PointerEnter: 3, PointerLeave: 4,
    FocusGained: 5, FocusLost: 6, KeyDown: 7, KeyUp: 8, Custom: 9,
} as const;

export const Positioning = { Flow: 0, Absolute: 1 } as const;
export const Flow = { None: 0, Row: 1, Column: 2 } as const;

const kRoot = 0;

// --- the C++-invoked dispatch callback + the authored-handler registry ---------------------------
export interface UiEvent { type: number; x: number; y: number; }
type Handler = (ev: UiEvent) => void;

const handlers: Handler[] = [];
(globalThis as any).__ui_handlers = handlers;

// The host calls this (JsEngine::callFunction) when a node a handler was registered on receives a
// matching event; it looks the authored handler up by id and runs it in-VM.
(globalThis as any).__ui_invoke = (id: number, type: number, x: number, y: number): number => {
    const fn = handlers[id];
    if (fn) {
        fn({ type, x, y });
    }
    return 0;
};

// --- the authored node handle (a thin fluent wrapper over a NodeId; CSS-like style props) ---------
export interface LayoutOpts {
    position?: number; // Positioning.*
    w?: number;
    h?: number;
    flow?: number;     // Flow.*
    gap?: number;
}

export class UiNode {
    constructor(public readonly id: number) {}

    opacity(v: number): this { __ui_set_opacity(this.id, v); return this; }
    visible(v: boolean): this { __ui_set_visible(this.id, v ? 1 : 0); return this; }
    background(r: number, g: number, b: number, a = 255): this {
        __ui_set_background(this.id, r, g, b, a);
        return this;
    }
    foreground(r: number, g: number, b: number, a = 255): this {
        __ui_set_foreground(this.id, r, g, b, a);
        return this;
    }
    padding(p: number): this { __ui_set_padding(this.id, p); return this; }
    layout(opts: LayoutOpts): this {
        __ui_set_layout(this.id, opts.position ?? Positioning.Flow, opts.w ?? 0, opts.h ?? 0,
                        opts.flow ?? Flow.None, opts.gap ?? 0);
        return this;
    }
    bounds(x: number, y: number, w: number, h: number): this {
        __ui_set_bounds(this.id, x, y, w, h);
        return this;
    }
    // Read-only data binding: this node's displayed value tracks the numeric state `key`.
    bindValue(key: number): this { __ui_bind_value(this.id, key); return this; }
    on(type: number, fn: Handler): this {
        const id = handlers.length;
        handlers.push(fn);
        __ui_on(this.id, type, id);
        return this;
    }
    onClick(fn: Handler): this { return this.on(EventType.PointerDown, fn); }
}

// --- the authoring API surface --------------------------------------------------------------------
export const ui = {
    root(): UiNode { return new UiNode(kRoot); },
    create(parent: UiNode, role: number): UiNode { return new UiNode(__ui_create(parent.id, role)); },
    panel(parent: UiNode): UiNode { return this.create(parent, Role.Panel); },
    group(parent: UiNode): UiNode { return this.create(parent, Role.Group); },
    label(parent: UiNode): UiNode { return this.create(parent, Role.Label); },
    button(parent: UiNode): UiNode { return this.create(parent, Role.Button); },
    progressBar(parent: UiNode): UiNode { return this.create(parent, Role.ProgressBar); },

    // State: the UI->state action path (write/add) + the read-only data-binding read source.
    writeState(key: number, value: number): number { return __ui_write_state(key, value); },
    addState(key: number, delta: number): number { return __ui_add_state(key, delta); },
    readState(key: number): number { return __ui_read_state(key); },
};

// Install a `context.ui` global too — the R-UI-001 surface name authors reach for.
(globalThis as any).context = (globalThis as any).context ?? {};
(globalThis as any).context.ui = ui;
