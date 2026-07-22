// The editor-core T1 unit-test harness (M9 e07a, design 09 §1 / 04). A TINY in-repo assert + test
// runner — deliberately NOT a test-runner package: the webui workspace acquires every third-party
// input through a SHA-pinned, fail-closed fetch channel (../../README.md § the one rule), and a
// test runner would be a new npm dependency re-triggering the 08 §3 supply-chain gate (the allowlist
// is dockview-core@7.0.2 ONLY). esbuild bundles this shim plus the sibling `*.test.ts` modules into
// ONE browser bundle (`editor-core.test.js`); `tools/webui_test_run.py` runs that bundle in the
// headless Chromium the CI already provisions and reads back the POSTed verdict.
//
// The tier proves the PURE, DOM-free editor-core logic (the total parsers in panels.ts / editorstate.ts,
// the structural guards in bridge.ts / dockview.ts) that today has no local TS test tier — live TS is
// otherwise proven ONLY by the CI-only `editor-cef-smoke-shell` leg (test.md § webui TS/DOM runtime
// blind spot). Every later editor-core TS task (e07b/e07c/e07d) declares `T1-tested` DoD items that run
// HERE.

/** One unit test: a name plus a body that throws (via `assert*`) on failure. */
export interface TestCase {
    readonly name: string;
    readonly run: () => void;
}

/** One test that threw, with its message — carried back to the driver so a red leg names the cause. */
export interface TestFailure {
    readonly name: string;
    readonly error: string;
}

/** The whole run's verdict. `failed === 0` is the ONLY pass condition the driver accepts. */
export interface TestSummary {
    readonly passed: number;
    readonly failed: number;
    readonly failures: readonly TestFailure[];
}

/** A thrown assertion — distinguished from an unexpected runtime error only by its name. */
export class AssertionError extends Error {
    constructor(message: string) {
        super(message);
        this.name = "AssertionError";
    }
}

/** Assert a condition, throwing `message` when it does not hold. */
export function assert(condition: boolean, message: string): void {
    if (!condition) {
        throw new AssertionError(message);
    }
}

/**
 * Assert deep structural equality of two JSON-shaped values (the parsers return plain
 * objects/arrays/primitives). Key ORDER is ignored; `message` names the expectation, and the
 * mismatch is rendered into the error so a failing leg reports what differed, not just that it did.
 */
export function assertEqual(actual: unknown, expected: unknown, message: string): void {
    if (!deepEqual(actual, expected)) {
        throw new AssertionError(
            `${message}: expected ${stringify(expected)}, got ${stringify(actual)}`,
        );
    }
}

/** Assert a value is strictly `null` — the parsers' documented "unreadable envelope" signal. */
export function assertNull(actual: unknown, message: string): void {
    if (actual !== null) {
        throw new AssertionError(`${message}: expected null, got ${stringify(actual)}`);
    }
}

/**
 * Run every test, isolating failures: one throwing test is recorded and the run continues, so a
 * single break never hides the rest. Returns the verdict rather than reporting it — the entry module
 * owns transport (posting it to the driver).
 */
export function runTests(tests: readonly TestCase[]): TestSummary {
    let passed = 0;
    const failures: TestFailure[] = [];
    for (const test of tests) {
        try {
            test.run();
            passed += 1;
        } catch (error) {
            failures.push({ name: test.name, error: describeError(error) });
        }
    }
    return { passed, failed: failures.length, failures };
}

function describeError(error: unknown): string {
    if (error instanceof Error) {
        return error.message;
    }
    return stringify(error);
}

function stringify(value: unknown): string {
    try {
        return JSON.stringify(value) ?? String(value);
    } catch {
        return String(value);
    }
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

function deepEqual(a: unknown, b: unknown): boolean {
    if (a === b) {
        return true;
    }
    if (Array.isArray(a) || Array.isArray(b)) {
        if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) {
            return false;
        }
        return a.every((item, index) => deepEqual(item, b[index]));
    }
    if (isPlainObject(a) && isPlainObject(b)) {
        const keysA = Object.keys(a);
        const keysB = Object.keys(b);
        if (keysA.length !== keysB.length) {
            return false;
        }
        return keysA.every(
            (key) => Object.prototype.hasOwnProperty.call(b, key) && deepEqual(a[key], b[key]),
        );
    }
    return false;
}
