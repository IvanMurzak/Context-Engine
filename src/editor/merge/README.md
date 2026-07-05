# `src/editor/merge/` — schema-aware structural three-way merge (R-FILE-012)

The convergence primitive for **worktree-per-agent parallelism** (L-26/L-50): a field-path-granular,
id-identity three-way merge over authored JSON. Line-based merging of JSON scenes produces spurious
conflicts and silent structural corruption; a schema-aware driver — resting on the stable intra-file
ids (L-33) and id-path overrides (L-35) — avoids both. This is what lets N agents author the same
game in parallel worktrees and converge via `git merge` without text-conflict corruption.

## What it guarantees

- **Field-path granularity.** Disjoint edits on two branches (ours edits one field, theirs another)
  auto-merge. Only genuinely overlapping edits conflict.
- **Id-based merge identity (L-33).** Id-keyed arrays (arrays of objects with a stable `id`) merge by
  id, not by position: reordering one side and field-editing the other converges cleanly. The **same
  id ADDED on both sides vs base is a structural conflict** (`id_add_add`) — never silently unified.
- **Never last-writer-wins, never text markers.** An unresolvable divergence becomes a machine-readable
  `Conflict`, and the merged output stays valid JSON (the conflicting field holds a deterministic OURS
  placeholder until resolved) — never `<<<<<<<` markers that would break every downstream JSON tool.
- **Whole-file conflict classes** for data that must never be field-blended: binary sidecars
  (`binary_sidecar`), meta files where both sides minted a different GUID (`meta_guid`), and inputs
  stamped newer than the installed schemas (`newer_stamped`) — the last is a *named class*, never a
  parse error (L-37 / R-FILE-012(a)/(d)).

## Surface

- `merge_documents(base, ours, theirs, options)` — the core (three_way_merge.h). Returns the merged
  tree + a list of `Conflict{path, class, id?, base?, ours?, theirs?}`.
- `find_duplicate_ids(root)` / `rekey_entity(root, pointer, new_id)` — the post-merge convergence gate
  (rekey.h, R-FILE-012(c)): the duplicate-intra-file-id diagnostic and the re-key operation (mint a
  fresh id + rewrite unambiguous in-file `{"$entity": …}` references).

## CLI verbs (src/cli/merge_command.cpp)

- `context merge-file <base> <ours> <theirs> [--output F] [--driver]` — run the merge; write the
  merged file; emit the R-CLI-008 conflict envelope (`conflicts: [{path, base, ours, theirs}]`). With
  `--driver` it is the git merge-driver entry point (writes the result to the ours/`%A` path, exits
  non-zero on unresolved conflicts, drops a `<file>.ctxconflicts.json` sidecar for the resolve loop).
- `context resolve-conflict <file> --path P --take ours|theirs | --value <json>` — apply one
  resolution against the merge output + its conflict sidecar; loop until the sidecar is empty.
- `context re-key <file> --at <pointer> | --id <id>` — mint a fresh id for a duplicated entity and
  rewrite its in-file references.
- `context validate [path]` — the convergence gate: report `merge.duplicate_id` diagnostics.

`context new` auto-installs the driver: it writes the `.gitattributes` mapping (`*.json merge=context`)
and the `[merge "context"]` driver definition (L-27: the ENGINE never invokes git; git invokes the
driver). See `scaffold.cpp`.

## Layering

Depends on `context_serializer` (parse/canonical/JsonValue) and `context_compose` (L-35 RFC 6901
pointer resolution + L-33 stable-id mint/recognition) — never the reverse. No dependency on the
schema/contract/derivation layers; the CLI supplies the installed-schema floor as plain data.
