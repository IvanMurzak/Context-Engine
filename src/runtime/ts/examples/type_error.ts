// A SYNTACTICALLY VALID but type-INVALID module (issue #85). esbuild transpiles this fine — it strips
// the type annotations without checking them (ts.transpile_failed would need a SYNTAX error, see
// bad_syntax.ts) — so only a tsc-class semantic typecheck (tsgo --noEmit) rejects it, surfacing each
// error as ts.type_error. This is the exact gap the author->typecheck->fix loop closes.

export const answer: number = "forty-two"; // TS2322: string is not assignable to number

function add(a: number, b: number): number {
    return a + b;
}

export const bad = add(1, "two"); // TS2345: string argument is not assignable to a number parameter
