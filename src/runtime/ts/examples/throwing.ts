// An authored-TS entrypoint that throws at module load — the R-OBS-005 fixture. Bundled + eval'd
// in the V8 host, `detonate` throws, and the raw V8 JS stack (positions into the transpiled bundle)
// must remap back to THIS file's positions via the esbuild-emitted Source Map v3 (source_map.h /
// stack_trace.h). Keep the throw on its own line so the mapped position is unambiguous.

function detonate(reason: string): never
{
    throw new Error(reason);
}

detonate("kaboom");
