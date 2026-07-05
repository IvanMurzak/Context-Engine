// Minimal save-migration runner (R-DATA-005 / L-37): a shipped build loads OLDER player saves by
// migrating each per-component payload from its recorded schemaVersion up to the build's current
// version — through EXACTLY the per-payload migration mechanism the editor uses at parse time
// (migrate::migrate_payload), never a separate copy. Builds embed migrations for the compiled
// component set only; the runner declares a bounded back-compat scope (N versions) rather than an
// unbounded promise, and enforces the L-37 execution contract (tier gating, budget, purity, id
// immutability). Composed identity (the save's addressing key) survives the upgrade untouched.

#pragma once

#include "context/editor/migrate/migrate_document.h"
#include "context/editor/migrate/migration_set.h"
#include "context/runtime/save/save_document.h"

#include <cstdint>
#include <vector>

namespace context::runtime::save
{

namespace migrate = context::editor::migrate;

struct MigrateSaveOptions
{
    // The RUNNING build's declared back-compat scope (N versions, R-DATA-005): a component payload
    // stamped more than N versions behind the current schema is REFUSED (save.back_compat_exceeded)
    // rather than migrated. This is the loader's capability — it governs regardless of the scope the
    // writing build recorded in the save header. Default: the engine-wide kDefaultBackCompatScope.
    std::int64_t back_compat_scope = kDefaultBackCompatScope;
    // The per-payload migration budget (L-37): passed straight through to migrate::migrate_payload.
    migrate::MigrationBudget budget{};
};

struct SaveMigrationResult
{
    bool ok = true;      // false iff a BLOCKING finding was hit (the save was rolled back untouched)
    bool changed = false; // true iff at least one payload was migrated + the header re-stamped
    std::vector<migrate::MigrationDiagnostic> diagnostics;
};

// Migrate a parsed save forward to `set` (the running build's compiled component set). For each
// component type the save header stamps:
//   - unregistered in this build (current == 0)  — save.unknown_component (blocking): the save
//     carries a component this build does not include (R-DATA-005 compiled-set rule).
//   - stamped older than the declared back-compat scope  — save.back_compat_exceeded (blocking).
//   - otherwise  — every entity's payload of that type is migrated via migrate::migrate_payload
//     (which handles the current/older/newer cases and the full L-37 contract), and the header is
//     re-stamped to the current version.
// A component payload present on an entity but NOT stamped in the header is save.malformed
// (blocking) — a save is self-describing. All-or-nothing: any blocking finding rolls the whole save
// back to its pre-call state and leaves ok == false (last-good; never a partial load).
[[nodiscard]] SaveMigrationResult migrate_save(SaveDocument& save, const migrate::MigrationSet& set,
                                               const MigrateSaveOptions& options = {});

} // namespace context::runtime::save
