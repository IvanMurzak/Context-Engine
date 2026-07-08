// Authored TypeScript gameplay system (M3 keystone — R-LANG-002/009/010). Proves the headline DoD:
// gameplay authored in TS mutates the SHARED World through the zero-copy (query, executor) path.
//
// It imports the DERIVED accessor for the `demo:mover` component (generated from that component's
// declarative schema by accessor_codegen.h, written next to this file at test time), and exposes a
// (query, executor) EXECUTOR on globalThis. The host resolves that executor and runs it ONCE via
// runSystemView over a zero-copy view of the mover column: the accessor's typed get/set land straight
// in the gathered VM bytes, which the host scatters back into the World. Bundled with esbuild
// `--bundle --format=iife`, the top-level assignment self-executes when the host evaluates the module.

import { demo_mover } from "./accessor";

// The (query, executor) executor: advance each mover's x-position by its speed and tick its hit
// counter — a minimal but real gameplay step over every selected entity's record.
(globalThis as any).__moveSystem = function (view: Uint8Array): void
{
    // Stash the view on purpose: the R-LANG-009 gate must NEUTER it at system exit, so a post-run
    // read of its byteLength is 0 (the host asserts this — a retained view may not survive the call).
    (globalThis as any).__lastMoverView = view;

    const m = new demo_mover(view);
    for (let row = 0; row < m.count; row++)
    {
        m.setPos(row, 0, m.getPos(row, 0) + m.getMaxSpeed(row));
        m.setHits(row, m.getHits(row) + 1);
    }
};
