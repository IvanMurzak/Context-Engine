// Authored TypeScript gameplay for the platformer sample (R-LANG-002/009/010; M6 X1 R-SIM-008).
//
// Gameplay in Context is written once in TypeScript and runs on the embedded JS VM. A system is a
// (query, executor) pair: the query selects entities by the components they carry; the host gathers
// those columns into a zero-copy view, evaluates the executor once per tick, and scatters the
// mutated bytes back into the shared World. This file authors the platformer's REAL per-tick player
// control — the same run+jump response the native mirror in src/tests/integration/games/ steps and
// the game-smoke-platformer-2d gate asserts — and it is the sustained-run subject of the M6 exit
// GC-budget assertion, so it follows the X1 pooled/no-allocation discipline (`ctx` hot API):
//   * ZERO per-tick allocation — scalar locals only, no object/array literals in the tick path;
//   * SIM-path math in Q16.16 fixed point via `ctx.fixed` (bit-exact with the C++ simmath core),
//     never JS floats — the platformer's world state is integer/fixed-point end to end (L-54).

/** Q16.16 fixed-point constants (raw): the platformer's tuning values. */
const FIXED_ONE = 65536; // 1.0 in Q16.16
const MAX_RUN_SPEED = 4 * FIXED_ONE; // horizontal top speed, units/second
const RUN_ACCEL = FIXED_ONE >> 3; // per-tick velocity step toward the target (1/8 unit/s)
const JUMP_VELOCITY = 6 * FIXED_ONE; // instantaneous vertical take-off speed
const GROUNDED_EPSILON = FIXED_ONE >> 2; // |vy| below this counts as standing on a surface

/**
 * The per-tick player control system: accelerate the horizontal velocity toward
 * `move_x * MAX_RUN_SPEED` (bounded by RUN_ACCEL per tick, so a held direction ramps up instead of
 * teleporting), and convert a grounded jump press into an instantaneous vertical take-off. All
 * values are Q16.16 raw integers straight from the zero-copy view — no floats touch sim state.
 */
export function playerControlSystem(view: PlayerView, input: InputStateView): void
{
    const moveX = input.moveX(); // authored axis value, whole units (-3..3)
    const jumpHeld = input.jumpButton() !== 0;
    for (let row = 0; row < view.count; row++)
    {
        // Horizontal: bounded acceleration toward the input's target speed (Q16 exact).
        const target = ctx.fixed.mul(moveX * FIXED_ONE, MAX_RUN_SPEED);
        const vx = view.getVelX(row);
        let step = target - vx;
        if (step > RUN_ACCEL)
            step = RUN_ACCEL;
        else if (step < -RUN_ACCEL)
            step = -RUN_ACCEL;
        view.setVelX(row, vx + step);

        // Vertical: a grounded jump press becomes an instantaneous take-off velocity.
        const vy = view.getVelY(row);
        const grounded = vy > -GROUNDED_EPSILON && vy < GROUNDED_EPSILON;
        if (jumpHeld && grounded)
            view.setVelY(row, JUMP_VELOCITY);
    }
}

/** The zero-copy accessor the host generates from the player body's declarative schema (Q16 raw). */
export interface PlayerView
{
    readonly count: number;
    getVelX(row: number): number;
    setVelX(row: number, rawQ16: number): void;
    getVelY(row: number): number;
    setVelY(row: number, rawQ16: number): void;
}

/** The world-singleton InputState view: the mapped action channels the input system folds. */
export interface InputStateView
{
    /** The held move_x axis value (whole units, the authored bindings' value domain). */
    moveX(): number;
    /** The held jump button channel (0 = released). */
    jumpButton(): number;
}

/** The engine-provided pooled hot API (installed as `globalThis.ctx` — see src/runtime/ts/). */
declare const ctx: {
    fixed: {
        /** Q16.16 multiply, bit-exact with the C++ simmath core. */
        mul(aRawQ16: number, bRawQ16: number): number;
    };
};
