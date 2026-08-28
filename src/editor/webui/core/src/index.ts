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
export * from "./extpanel.js";
export * from "./hydration.js";
export * from "./panelhost.js";
export * from "./window.js";
// editor-window-chrome a2 — the four-strip frame, the titlebar content, and the first real
// regionProvider. Re-exported for the same load-bearing build reason as the rest of the barrel:
// `webui-assets` asserts the entry's symbols survive into the bundle, and the DOM tier drives the
// mount/publisher through these exports.
export * from "./chrome.js";
export * from "./editorstate.js";
export * from "./when.js";
export * from "./commands.js";
export * from "./keymap.js";
export * from "./theme.js";
export * from "./config.js";
export * from "./settings.js";
export * from "./palette.js";
export * from "./palette_view.js";
// M9 e09b-3 — the LOUD refused-write surface. Re-exported for the same load-bearing build reason the
// rest of this barrel is: `webui-assets` asserts the entry's symbols survive into the bundle, and the
// `webui-panel-contract` gate re-reads `WRITE_NOTICE_KIND_*` out of that bundle to compare them
// against write_notice.h — a symbol the entry does not name could be tree-shaken away, which would
// turn a drift check into a check that silently stopped running.
export * from "./notifications.js";
export * from "./boot.js";

import { bootEditorCore } from "./boot.js";

// Fire-and-forget on load: `bootEditorCore` never throws and never rejects (it reports through its
// return value and a `data-editor-core` attribute on <html>), so `void` here discards a promise
// that cannot reject rather than swallowing errors. Awaiting it is not an option — this is a module
// body, and a top-level await would delay every importer for one IPC round trip.
void bootEditorCore();
