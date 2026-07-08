// R-OBS-005 source-mapped-breakpoint fixture (issue #94). An authored TypeScript function whose
// body line is the breakpoint target. Bundled IIFE, step() is invoked at module load, so a
// breakpoint set on the "n * 2" line (resolved authored-TS -> generated-JS through the emitted
// Source Map v3) executes and pauses. The test scans this file for the "n * 2" token to find the
// breakpoint's authored line, so editing the layout here does not break the test.

export function step(n: number): number
{
    const marked = n * 2;
    return marked + 1;
}

globalThis.__bp_result = step(20);
