// Authored TypeScript gameplay entrypoint (issue #83, task 2b-i). Proves "authored TS runs in
// the shipped V8 VM" and exercises the host<->TS boundary BOTH ways:
//   * JS -> host: at module top level this TS calls an engine-provided host function
//     (`hostBias`) — the host binds it before evaluating the bundle.
//   * host -> JS: this TS exports an entrypoint (`globalThis.update`) that the host resolves
//     and calls each frame.
// It also imports a sibling module (util.ts) so the toolchain's `--bundle` mode is exercised on
// a real cross-file import. Bundled with esbuild `--bundle --format=iife`, the whole thing
// becomes a self-executing module: the top-level `hostBias` call and the `globalThis.update`
// assignment run when the runtime/js host evaluates the emitted JS.

import { scale } from "./util";

// Engine-provided host function (bound by the host as a global before eval). Declaring it keeps
// the TypeScript well-typed without importing an ambient engine .d.ts (that lands with the
// R-LANG-010 accessor codegen in task 2b-ii).
declare function hostBias(seed: number): number;

// JS -> host: read a per-run bias from the engine at load time.
const bias: number = hostBias(7);

// host -> JS: the gameplay update the host drives. Returns a value derived from BOTH the bundled
// pure-TS helper (`scale`) and the host-provided bias, so the round trip is observable.
(globalThis as any).update = (x: number): number => scale(x) + bias;
