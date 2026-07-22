// Dev-only regenerator for ../theme.schema.json (M9 e06a). Emits the published JSON Schema from the
// single source of truth, src/schema.ts's toJsonSchema(). NOT a CI step: the committed theme.schema.json
// is guarded by the T1 drift test (core/src/test/tokens.test.ts), which asserts toJsonSchema() is
// byte-identical to the committed file — so this script only needs to run when the schema changes.
//
//   node --experimental-strip-types src/editor/webui/tokens/scripts/gen-schema.mjs
//
// (Node >= 22.18 strips the imported TypeScript types; no build step, no npm install.)
import { writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { toJsonSchema } from "../src/schema.ts";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(here, "..", "theme.schema.json");
// Two-space indent + trailing newline — the same shape the *.theme.json data files use, so the
// tree stays visually consistent and diffs stay small.
writeFileSync(out, JSON.stringify(toJsonSchema(), null, 2) + "\n");
console.log(`wrote ${out}`);
