# src/kernel/

The **microkernel** — the deliberately tiny stable core (~6 interfaces) that everything else —
including rendering, physics, and audio — plugs into as packages (R-KERNEL-001/002). Governed by the
Context Engine design records: **ARCHITECTURE.md** (component breakdown, microkernel + packages
principle), **ROADMAP.md §1 M1**, and **DESIGN-DECISIONS.md** (L-60 custom archetype/SoA World, L-39
sim→render timing).

## The interfaces

Public headers live under `include/context/kernel/`; implementations under `src/`. Everything is in
namespace `context::kernel`. The `Kernel` facade (`kernel.h`) bundles exactly these and nothing with
game-feature semantics (R-KERNEL-001).

- **World** (`world.h`, `entity.h`, `component.h`) — the data-oriented, **custom archetype/SoA** ECS
  (L-60, R-SIM-003). Stable generation-checked entity ids; create/destroy; add/remove/get
  components with archetype migration; cache-friendly column queries (`each<Cs...>`).
- **Scheduler** (`scheduler.h`) — the **fixed-timestep** loop and the sim→render timing contract
  (R-SIM-002, L-39): accumulator-based fixed ticks, interpolation `alpha`, tick-rate policy, and the
  high-refresh presentation rule. Full contract in
  [`docs/sim-render-timing-contract.md`](../../docs/sim-render-timing-contract.md).
- **Module registry** (`module.h`) — the uniform package contract + **explicit** registration seam
  (R-KERNEL-003/004). No static-initializer self-registration (it would defeat DCE); a generated TU
  registers exactly the referenced packages in shipped builds.
- **Event bus** (`event_bus.h`) — kernel-internal typed pub/sub, including the built-in `log` topic.
  Distinct from the client-facing event *stream* (R-BRIDGE-008).
- **Resource handles** (`resource.h`) — typed, **generation-checked** handles into a `ResourcePool`;
  a stale handle resolves to `nullptr` rather than aliasing a reused slot.
- **Platform seam** (`platform.h`) — injectable `Clock` / `FileSystem` / `TaskRunner` interfaces that
  keep the kernel GPU-less and headless (R-HEAD-001) and give the M1 fault-injection harness its
  hooks (R-QA-010). Ships minimal impls (`SteadyClock`, `ManualClock`, `MemoryFileSystem`,
  `InlineTaskRunner`).

## Build & test

`context_kernel` is a static library, always-on from M1 (`add_subdirectory(kernel)` in
`src/CMakeLists.txt`). Tests are dependency-free assertion executables registered with ctest
(`kernel-test_world`, `kernel-test_scheduler`, …), each covering happy path, edge cases, and failure
paths (R-QA-013). Build + run via the standard dev gate:

```bash
cmake -S src --preset dev
cd src && cmake --build --preset dev && ctest --preset dev --output-on-failure
```
