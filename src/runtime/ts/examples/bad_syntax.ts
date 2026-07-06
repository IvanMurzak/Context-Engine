// Deliberately malformed TypeScript — a transpile-error fixture. esbuild exits non-zero, which
// the toolchain surfaces as a `ts.transpile_failed` diagnostic (R-CLI-008). NOT a build input.
const x: number = ;
