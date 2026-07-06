// A trivial gameplay helper module, imported by game.ts. Exists to prove the toolchain's
// `--bundle` mode resolves + inlines a real cross-file TypeScript import (issue #83).

export function scale(x: number): number
{
    return x * 3;
}
