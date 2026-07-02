# kernel/

The **microkernel** — the deliberately tiny stable core (~6 interfaces: World/ECS, fixed-timestep
Scheduler, module registry, event bus, resource handles, platform seam) that everything else —
including rendering, physics, and audio — plugs into as packages. Nothing lands here until M1.
Governed by the Context Engine design records: **ARCHITECTURE.md** (component breakdown,
microkernel + packages principle) and **ROADMAP.md §1 M1**.
