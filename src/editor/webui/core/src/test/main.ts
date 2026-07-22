// The editor-core T1 test bundle ENTRY (M9 e07a). esbuild bundles this into `editor-core.test.js`;
// `tools/webui_test_run.py` serves it in headless Chromium, waits for the POST below, and turns the
// `webui-ts-unit` ctest RED when `failed > 0`. Kept as thin as core/src/index.ts's own entry: gather
// the test modules, run them, report the verdict. New editor-core TS modules add their tests by
// exporting a `TestCase[]` and appending it here.

import { runTests, type TestSummary } from "./harness.js";
import { panelsTests } from "./panels.test.js";
import { editorstateTests } from "./editorstate.test.js";
import { guardsTests } from "./guards.test.js";

const result = runTests([...panelsTests, ...editorstateTests, ...guardsTests]);
report(result);

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
