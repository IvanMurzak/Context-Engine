// Parse-time document migration (L-37 / R-DATA-004): read the per-component-payload
// `componentVersions` stamps, select each stamped type's migration chain from the registered set,
// apply it IN MEMORY — the engine reads old versions by migrating at parse time WITHOUT touching
// disk; the only migration that ever writes disk is the explicit `context migrate` bulk verb (and
// tool saves, which canonicalize + stamp the file they were going to write anyway).

#pragma once

#include "context/editor/migrate/migration_set.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::migrate
{

// The sandboxed-migration execution seam (migration_runner.h). Forward-declared: this header only
// takes a `const MigrationRunner*` (a nullable pointer — a caller that INJECTS a runner includes
// migration_runner.h, but a caller that migrates only engine-native payloads needs no VM header).
class MigrationRunner;

// One machine-readable migration finding. `code` is drawn from the R-CLI-008 catalog
// (migration.step_missing / migration.step_failed / migration.budget_exceeded /
// migration.id_mutated / migration.runner_unavailable / migration.orphan_override /
// schema.newer_than_engine / schema.newer_than_package); `pointer` addresses the offending value
// in the DOCUMENT (empty = whole document). Line/column resolution against source bytes is the
// caller's concern (the derivation validate layer owns locate_pointer) — this module depends only
// on the serializer.
struct MigrationDiagnostic
{
    std::string code;
    std::string message;
    std::string pointer;
    bool blocking = true; // false: informational (orphan overrides); true: last-good semantics
};

struct MigrateOptions
{
    // Tool-save / bulk-path stamping (L-37: "tool saves canonicalize the whole file they write and
    // stamp current schema versions"): ALSO stamp `componentVersions` entries for every REGISTERED
    // component type present as a payload site but carrying no stamp (an unstamped payload of a
    // registered type is by definition authored at the current version — a from-version is exactly
    // what a stamp records). Parse-time (derivation) migration leaves this false: it migrates
    // stamped payloads only and never invents header entries in the in-memory view.
    bool stamp_registered_sites = false;
    MigrationBudget budget{};

    // The sandboxed-migration execution seam (L-37 / issue #71). NULL (the default) keeps the
    // honest migration.runner_unavailable refusal for every package_sandboxed step — the parse-time
    // engine never runs package migrations unsandboxed. When a runner IS injected (the wasmtime
    // runtime, PR3 of the #71 chain), package_sandboxed steps are routed THROUGH it under the same
    // host-side contract the engine_native tier obeys (budget, canonical, id immutability). Owned by
    // the caller; must outlive the migrate_document call. Non-const: a runner is a stateful executor
    // (the wasmtime backend caches a module/store) — running a step may mutate its internals.
    MigrationRunner* runner = nullptr;
};

// The outcome of migrating one document in memory.
//
//   ok == true, changed == false — nothing to do (no stamps, everything current, or no registered
//                                  types present). The tree is untouched.
//   ok == true, changed == true  — at least one payload migrated and re-stamped (and/or a stamp
//                                  was added under stamp_registered_sites). Non-blocking findings
//                                  (migration.orphan_override) may be present.
//   ok == false                  — a BLOCKING finding (missing/failed/over-budget step, id
//                                  mutation, sandboxed-tier refusal, newer-than stamps). The tree
//                                  is ROLLED BACK to its pre-call state (all-or-nothing per
//                                  document): derivation retains last-good derived state
//                                  (R-FILE-003) and the bulk path does not write the file.
struct DocumentMigrationResult
{
    bool ok = true;
    bool changed = false;
    std::vector<MigrationDiagnostic> diagnostics;
};

// Migrate a parsed authored document in place against `set`.
//
// Selection (per `componentVersions` entry "<ns>:<type>" -> stamped v, current c = registered):
//   type unregistered (c == 0)  — untouched (packages register incrementally; not ours to judge).
//   v == c                      — current; nothing to do.
//   v <  c                      — apply the step chain v → v+1 → … → c to EVERY payload site of
//                                 the type; a gap in the chain is migration.step_missing.
//   v >  c                      — the L-37 downgrade rule: schema.newer_than_engine (engine "ctx"
//                                 namespace) / schema.newer_than_package (any other namespace),
//                                 blocking — never a best-effort parse (R-PKG-005).
//
// A payload SITE is any object member whose key equals a stamped (or, under
// stamp_registered_sites, registered) component type and whose value is an object — the
// document-shape-agnostic rule that works for any kind carrying namespaced component payloads.
// The root `componentVersions` header object itself is exempt from site scanning, and payload
// subtrees are OPAQUE to every document traversal (site discovery, the unstamped-site scan,
// override rewriting): no traversal descends into ANY stamped/registered type's payload, so
// payload-internal data that happens to use a namespaced key or an "overrides"-shaped member is
// never misread as document structure.
//
// Around every step invocation the engine enforces the L-37 execution contract:
//   - tier gating: package_sandboxed steps run ONLY through the injected sandboxed MigrationRunner
//     (MigrateOptions::runner — the wasmtime runtime, issue #71). With NO runner injected they are
//     REFUSED in-process (migration.runner_unavailable) — never run unsandboxed. With one injected
//     the step is routed to the guest per the frozen ABI (migration_runner.h) under this SAME
//     contract (budget, canonical, id immutability re-checked host-side);
//   - budget: input/output payloads over budget.max_nodes are refused (migration.budget_exceeded);
//   - id immutability: the exact multiset of ("id"/"guid" member pointer, canonical value) inside
//     the payload must survive the step unchanged — no mutation, move, addition, or removal
//     (migration.id_mutated). Composed identity survives upgrade (L-37).
//
// Path transforms: after successful chains, override paths are rewritten through the chained
// per-step maps. An override site is any object member named "overrides" whose value is an array
// of objects carrying a string "path"; a path names a migrated type as one of its '/'-separated
// segments, and the segments after the type are the payload-relative pointer fed through the
// chain. A hop mapping to nullopt orphans the override: the entry is PRESERVED verbatim (truth is
// the author's; parse-time migration never destroys data) and a NON-blocking
// migration.orphan_override finding marks it for exclusion from flatten (L-37).
//
// On any blocking finding the WHOLE document rolls back (all-or-nothing), `ok` is false, and the
// diagnostics describe every finding gathered before the rollback.
[[nodiscard]] DocumentMigrationResult migrate_document(serializer::JsonValue& root,
                                                       const MigrationSet& set,
                                                       const MigrateOptions& options = {});

// Map ONE payload-relative JSON pointer of `component_type` through the chained per-step path
// maps from `from_version` up to the registered current version. Returns the mapped pointer, or
// nullopt when any hop unmaps it (the orphan case) OR the chain cannot be resolved (gap /
// unregistered / not older). Exposed for future reference-path surfaces (the compose/flatten layer
// resolves orphaned overrides against the same rule) and for tests.
[[nodiscard]] std::optional<std::string> transform_payload_path(const MigrationSet& set,
                                                                std::string_view component_type,
                                                                std::int64_t from_version,
                                                                std::string_view pointer);

// Migrate ONE component payload of `component_type` from `from_version` up to the registered
// current version, IN PLACE, under the full L-37 execution contract (tier gating, per-invocation
// budget, purity, id immutability — the same enforcement document migration applies at every
// payload site). This is the shared per-payload migration PRIMITIVE: the RuntimeKernel
// save-migration runner (R-DATA-005) reuses it so a shipped build loads older player saves through
// EXACTLY the mechanism the editor uses at parse time, rather than duplicating the contract.
//
// `site_pointer` locates the payload for diagnostics (the caller's document shape — a save entity's
// component pointer, a scene payload site). Behavior:
//   - component unregistered (current == 0)  — no-op, returns true (unknown types are the caller's
//                                              policy: packages/builds register incrementally).
//   - from_version == current                — no-op, returns true.
//   - from_version >  current                — the L-37 downgrade rule: schema.newer_than_engine
//                                              ("ctx:" namespace) / schema.newer_than_package,
//                                              blocking, returns false (never a best-effort parse).
//   - from_version <  current (>= 1)         — apply the step chain; a gap is migration.step_missing,
//                                              a step violation is the apply_step finding
//                                              (step_failed / budget_exceeded / id_mutated /
//                                              runner_unavailable). On ANY blocking finding the
//                                              payload is ROLLED BACK to its pre-call bytes.
// `runner` is the sandboxed-migration seam forwarded to every package_sandboxed step (default null:
// the honest migration.runner_unavailable refusal, so the RuntimeKernel save path loads player saves
// through EXACTLY the parse-time refusal until the runner lands). Returns true iff the payload is at
// the current version afterward.
[[nodiscard]] bool migrate_payload(const MigrationSet& set, std::string_view component_type,
                                   std::int64_t from_version, serializer::JsonValue& payload,
                                   const MigrationBudget& budget, std::string_view site_pointer,
                                   std::vector<MigrationDiagnostic>& diagnostics,
                                   MigrationRunner* runner = nullptr);

// Count the JSON nodes of a payload subtree (the budget metric: every value + every object member
// counts one). Exposed for tests pinning the budget rule.
[[nodiscard]] std::uint64_t node_count(const serializer::JsonValue& value) noexcept;

} // namespace context::editor::migrate
