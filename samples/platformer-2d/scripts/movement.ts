// Authored TypeScript gameplay system for the platformer sample (R-LANG-002/009/010).
//
// Gameplay in Context is written once in TypeScript and runs on the embedded JS VM. A system is a
// (query, executor) pair: the query selects entities by the components they carry; the host gathers
// those columns into a zero-copy view, evaluates the executor once, and scatters the mutated bytes
// back into the shared World. This sample file is few-shot corpus -- it shows the SHAPE an agent
// should emit for a movement system. (Execution wiring is exercised by the runtime's own TS suite;
// the samples smoke gate exercises the file-authoring contract surface, not TS evaluation.)

/** The horizontal movement system: advance each mover's x-position by its per-entity speed. */
export function movementSystem(view: MoverView): void
{
    for (let row = 0; row < view.count; row++)
    {
        const x = view.getPositionX(row);
        view.setPositionX(row, x + view.getSpeed(row));
    }
}

/** The zero-copy accessor the host generates from the mover component's declarative schema. */
export interface MoverView
{
    readonly count: number;
    getPositionX(row: number): number;
    setPositionX(row: number, value: number): void;
    getSpeed(row: number): number;
}
