// Authored TypeScript gameplay system for the M3 task-4a keystone (R-SIM-006 / R-LANG-011). It runs on
// the scheduler's SINGLE JS-VM lane (TS systems are excluded from the parallel DAG) while a C++ native
// system mutates a DIFFERENT World component in the same tick — proving a C++ system and a TS system
// are scheduled safely together.
//
// It imports the DERIVED accessor for the `demo:health` component (generated from that component's
// declarative schema by accessor_codegen.h, written next to this file at test time) and exposes a
// (query, executor) EXECUTOR on globalThis. The host runs it ONCE via runSystemView over a zero-copy
// view of the health column: the accessor's typed get/set land straight in the gathered VM bytes,
// which the host scatters back into the World. Bundled with esbuild `--bundle --format=iife`, the
// top-level assignment self-executes when the host evaluates the module.

import { demo_health } from "./accessor";

// The executor: tick each entity's health up by one — a minimal but real gameplay step.
(globalThis as any).__healthSystem = function (view: Uint8Array): void
{
    // Stash the view on purpose: the R-LANG-009 gate must NEUTER it at system exit, so a post-run read
    // of its byteLength is 0 (the host asserts this — a retained view may not survive the call).
    (globalThis as any).__lastHealthView = view;

    const h = new demo_health(view);
    for (let row = 0; row < h.count; row++)
    {
        h.setHp(row, h.getHp(row) + 1);
    }
};
