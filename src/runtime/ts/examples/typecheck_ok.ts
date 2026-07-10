// A type-VALID module: a tsc-class semantic typecheck (tsgo --noEmit) reports zero diagnostics. The
// companion to type_error.ts — together they prove the typecheck seam distinguishes type-valid from
// type-invalid authored TS (issue #85). Deliberately free of ambient/host declarations so the check is
// deterministic under the tool's default settings.

export function scale(x: number): number {
    return x * 3;
}

export const tripled: number = scale(14);
