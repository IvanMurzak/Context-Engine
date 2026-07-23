// The editor-core T1 test bundle ENTRY (M9 e07a). esbuild bundles this into `editor-core.test.js`;
// `tools/webui_test_run.py` serves it in headless Chromium, waits for the POST below, and turns the
// `webui-ts-unit` ctest RED when `failed > 0`. Kept as thin as core/src/index.ts's own entry: gather
// the test modules, run them, report the verdict. New editor-core TS modules add their tests by
// exporting a `TestCase[]` and appending it here.

import { runTests, type TestSummary } from "./harness.js";
import { panelsTests } from "./panels.test.js";
import { editorstateTests } from "./editorstate.test.js";
import { guardsTests } from "./guards.test.js";
import { whenTests } from "./when.test.js";
import { commandsTests } from "./commands.test.js";
import { keymapTests } from "./keymap.test.js";
import { welcomeTests } from "./welcome.test.js";
import { paletteTests } from "./palette.test.js";
import { reachabilityTests } from "./reachability.test.js";
import { tokensTests } from "./tokens.test.js";
import { themeTests } from "./theme.test.js";
import { themeDomTests } from "./theme_dom.test.js";
import { kitTests } from "./kit.test.js";
import { kitComponentTests } from "./kit_components.test.js";
import { configTests } from "./config.test.js";
import { settingsTests } from "./settings.test.js";
import { bannerTests } from "./banners.test.js";
import { uibusTests } from "./uibus.test.js";
import { sessionTests } from "./session.test.js";
import { bootTests } from "./boot.test.js";

// AWAITED since M9 e08d: `runTests` is async because the e08d boot cases drive the real, async
// `bootEditorCore` (see boot.test.ts on why a synchronously-reachable seam would not prove the
// wiring). Every pre-existing case stays synchronous and is unaffected.
void runTests([
    ...panelsTests,
    ...editorstateTests,
    ...guardsTests,
    ...whenTests,
    ...commandsTests,
    ...keymapTests,
    ...welcomeTests,
    ...paletteTests,
    ...reachabilityTests,
    ...tokensTests,
    ...themeTests,
    ...themeDomTests,
    ...kitTests,
    ...kitComponentTests,
    ...configTests,
    ...settingsTests,
    ...bannerTests,
    ...uibusTests,
    ...sessionTests,
    // LAST, deliberately: the boot cases drive the whole bundle against a mock Shell and leave real
    // boot state on the shared document (`data-editor-*`, the applied theme's custom properties), so
    // running them after every other case keeps that out of the others' way.
    ...bootTests,
]).then(report);

function report(summary: TestSummary): void {
    const body = JSON.stringify(summary);
    // Mirror the verdict into the DOM so the `--dump-dom` local-repro path (test.md § webui TS/DOM
    // runtime blind spot) can read it with no driver, and set the title as a compact human signal.
    if (typeof document !== "undefined") {
        document.title =
            summary.failed === 0 ? `webui-ts PASS ${summary.passed}` : `webui-ts FAIL ${summary.failed}`;
        const pre = document.createElement("pre");
        pre.id = "webui-ts-result";
        pre.textContent = body;
        document.body.appendChild(pre);
    }
    // The verdict channel the driver blocks on. `keepalive` so it survives the browser being reaped
    // the instant it arrives. A driver is not always present (a bare `--dump-dom` run), so a failed
    // POST is swallowed — the DOM copy above is the standalone readout.
    if (typeof fetch === "function") {
        void fetch("/report", { method: "POST", body, keepalive: true }).catch(() => {
            /* no driver listening (standalone --dump-dom run) — the DOM copy above is the readout */
        });
    }
}
