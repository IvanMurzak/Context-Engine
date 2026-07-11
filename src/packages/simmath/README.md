# src/packages/simmath/ — deterministic no-alloc sim math (R-SIM-008)

The **one shared fixed-point math library** every simulation package composes on — physics (3D/2D),
animation, particles, spline. It is a **package**, not kernel core: `R-KERNEL-001` stays minimal and
this composes on the microkernel like any other feature package (the kernel gains no dependency on it).

## Why it exists

The sim path is **integer / fixed-point only** (the inherited M6 invariant): the hierarchical state
hash folds fixed-width big-endian integers, so a world's digest is bit-identical on x86-64 **and**
macOS-ARM64 (`L-54`). If each sim package rolled its own `float` math it would risk a platform-divergent
result that turns the blocking L-54 determinism gate red. `simmath` gives every package **one**
deterministic, allocation-free math core instead — this is the **math half of `R-SIM-008`**'s
pooled/no-alloc APIs (the JS-tier query half lands in X1).

## What it provides

- **`Fixed`** (`fixed.h`) — a Q16 fixed-point scalar (`int64` in 2^16 sub-units). Add/sub/mul/div,
  integer-scaled ops, floor/trunc, abs/min/max/clamp/sign. Every operation is integer add/mul/shift —
  **no float, ever** — so results are bit-identical across the platform matrix. Multiply and
  integer-divide truncate toward negative infinity (a pure arithmetic shift, never an FP rounding mode).
- **Transcendentals** (`trig.h`) — `fixed_sqrt`, `fixed_sin`, `fixed_cos`, `fixed_tan`, `fixed_atan`,
  `fixed_atan2`. Computed from a bit-by-bit integer square root and fixed-point polynomial
  approximations with integer range reduction — **`std::sin`/`libm` is NEVER called on the sim path**
  (a platform libm result would diverge across the determinism matrix). Accuracy is a few parts in
  10^4 over the working range; determinism is exact.
- **`Vec2` / `Vec3`** (`vec.h`) — arithmetic, `dot`, `cross`, `length`/`normalized` (through
  `fixed_sqrt`), Hadamard product.
- **`Quat`** (`quat.h`) — Hamilton product, conjugate, `normalized`, `quat_from_axis_angle` (via the
  deterministic sin/cos), and `rotate` via the trig-free cross-product form.
- **`Transform`** (`transform.h`) — a TRS transform (translation + quaternion + non-uniform scale),
  `transform_point`, `transform_direction`, and `compose`.

## Usage

```cpp
#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"
#include "context/packages/simmath/trig.h"
using namespace context::packages::simmath;

const Fixed dt = Fixed::from_ratio(1, 60);      // 1/60 s, exact fixed-point (no float literal)
Vec3 v{Fixed::from_int(1), kZero, kZero};
v = v * dt;                                       // integer-only, deterministic
const Fixed s = fixed_sin(kHalfPi);              // ~1.0, computed without libm
```

## Determinism contract

- **No float on the sim path.** The library declares no `float`/`double` and calls no `libm`. The
  TEST files use `std::sin`/`std::sqrt` ONLY as a tolerance oracle (never linked into the sim path).
- **Bit-identical across platforms.** Because every op is integer arithmetic folded in fixed endian,
  the results feed the L-54 hierarchical state hash unchanged on Linux-x64 / Win-x64 / macOS-ARM64,
  and it is deterministic under the `deterministic` CMake preset (F0a).

## Design records

`R-SIM-008` (pooled/no-alloc sim APIs — math half), `R-KERNEL-001` / `L-60` (why it's a package, not
kernel), `R-SIM-005` / `L-54` (the determinism contract it upholds). See `engine-design/REQUIREMENTS.md`
and `DESIGN-DECISIONS.md`, and `.claude/plans/designs/2026-07-11-m6-core-systems-decomposition.md` §F0b.
