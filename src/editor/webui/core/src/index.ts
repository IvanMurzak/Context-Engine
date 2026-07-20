// @context-engine/editor-core — the app entry point (design 04 section 1).
//
// This is the module `context-editor://app/index.html` loads, and the file esbuild bundles into
// `editor-core.js`. Two jobs, in this order:
//
//   1. RE-EXPORT the whole workspace surface, so panels and the hydration runtime import the
//      contract from ONE place and can never hand-roll a divergent copy (the barrel discipline
//      e05a established for the generated client schema, now covering the bridge, the panel wire
//      contract, the hydration runtime and PanelHost too).
//   2. BOOT the bridge handshake on load and, once it completes, bring up the panels — e05c's and
//      e05d1's deliverables, asserted end to end from the native side by the
//      `editor-cef-smoke-shell` ctest.
//
// The re-exports are also load-bearing for the build: `webui-assets` asserts the entry's symbols
// survive into the bundle, and an export the entry does not name would be tree-shaken away.

export * from "./info.js";
export * from "./bridge.js";
export * from "./dockview.js";
export * from "./panels.js";
export * from "./hydration.js";
export * from "./panelhost.js";
export * from "./boot.js";

import { bootEditorCore } from "./boot.js";

// Fire-and-forget on load: `bootEditorCore` never throws and never rejects (it reports through its
// return value and a `data-editor-core` attribute on <html>), so `void` here discards a promise
// that cannot reject rather than swallowing errors. Awaiting it is not an option — this is a module
// body, and a top-level await would delay every importer for one IPC round trip.
void bootEditorCore();
