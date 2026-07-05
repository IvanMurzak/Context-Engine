// Minimal save-migration runner implementation (see save_migration.h).

#include "context/runtime/save/save_migration.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace context::runtime::save
{

using migrate::MigrationDiagnostic;
using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

[[nodiscard]] JsonValue* find_member(JsonValue& object, std::string_view key)
{
    if (object.type != JsonValue::Type::object)
        return nullptr;
    for (JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// JSON-pointer segment escaping (RFC 6901): "~" -> "~0", "/" -> "~1". A component type/key is
// file-controlled (a save is loadable and hand-editable), so a segment carrying '/' or '~' must be
// escaped to keep a diagnostic pointer a valid RFC 6901 reference — mirrors the sibling
// migrate_document.cpp's escape_segment.
[[nodiscard]] std::string escape_pointer_segment(std::string_view segment)
{
    std::string out;
    out.reserve(segment.size());
    for (const char c : segment)
    {
        if (c == '~')
            out += "~0";
        else if (c == '/')
            out += "~1";
        else
            out += c;
    }
    return out;
}

void push_diag(std::vector<MigrationDiagnostic>& diagnostics, std::string code, std::string message,
               std::string pointer)
{
    MigrationDiagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    d.pointer = std::move(pointer);
    d.blocking = true;
    diagnostics.push_back(std::move(d));
}

} // namespace

SaveMigrationResult migrate_save(SaveDocument& save, const migrate::MigrationSet& set,
                                 const MigrateSaveOptions& options)
{
    SaveMigrationResult result;

    // A save is self-describing: every entity component payload type MUST be stamped in the header
    // (otherwise its from-version is unknown). This is checked before anything is applied.
    for (std::size_t i = 0; i < save.entities.size(); ++i)
    {
        for (const JsonMember& comp : save.entities[i].components.members)
        {
            if (save.saved_version(comp.key) == nullptr)
            {
                push_diag(result.diagnostics, "save.malformed",
                          "entity component \"" + comp.key +
                              "\" is not stamped in the save header componentVersions",
                          "/entities/" + std::to_string(i) + "/components/" +
                              escape_pointer_segment(comp.key));
                result.ok = false;
            }
        }
    }
    if (!result.ok)
        return result; // nothing applied yet

    // --- selection (per componentVersions entry, authored order — deterministic) ----------------
    // Classify EVERY stamped type BEFORE touching a single payload, exactly as the sibling
    // migrate_document's selection pass does (migrate_document.cpp): gather every type-level finding
    // (unknown / newer-than / back-compat) across ALL types, THEN — if any is blocking — refuse the
    // whole save up front. Separating selection from application this way guarantees an
    // application-time failure on an earlier type can never suppress a LATER type's selection
    // diagnostic (which an interleaved single loop would drop when it broke on the earlier failure).
    // Nothing is mutated in this pass.
    struct EligibleType
    {
        std::string type;
        std::int64_t saved_version;
        std::int64_t current;
    };
    std::vector<EligibleType> to_migrate;
    for (const auto& [type, saved_version] : save.component_versions)
    {
        const std::int64_t current = set.current_version(type);
        if (current == 0)
        {
            push_diag(result.diagnostics, "save.unknown_component",
                      "the save carries component \"" + type +
                          "\" which this build's compiled component set does not include "
                          "(R-DATA-005)",
                      "/componentVersions/" + escape_pointer_segment(type));
            result.ok = false;
            continue;
        }
        if (saved_version > current)
        {
            // The save was written by a NEWER build (L-37 downgrade rule, reusing the schema.* codes
            // migrate_payload would emit per site — surfaced here so a header-only newer stamp with
            // no live payload is still refused, never silently accepted).
            const bool engine_kind = type.rfind("ctx:", 0) == 0;
            push_diag(result.diagnostics,
                      engine_kind ? "schema.newer_than_engine" : "schema.newer_than_package",
                      "\"" + type + "\" is stamped version " + std::to_string(saved_version) +
                          " but this build's schema is version " + std::to_string(current) +
                          "; the save was written by a newer build",
                      "/componentVersions/" + escape_pointer_segment(type));
            result.ok = false;
            continue;
        }
        if (saved_version < current && (current - saved_version) > options.back_compat_scope)
        {
            push_diag(result.diagnostics, "save.back_compat_exceeded",
                      "\"" + type + "\" is stamped version " + std::to_string(saved_version) +
                          " but this build is version " + std::to_string(current) +
                          ", beyond the declared " + std::to_string(options.back_compat_scope) +
                          "-version save back-compat scope (R-DATA-005)",
                      "/componentVersions/" + escape_pointer_segment(type));
            result.ok = false;
            continue;
        }
        // saved_version == current (a no-op) or older-within-scope: eligible for payload migration.
        to_migrate.push_back({type, saved_version, current});
    }
    if (!result.ok)
        return result; // every selection finding gathered; nothing was applied — no rollback needed

    // --- application (all-or-nothing per save: snapshot, apply, roll back on a blocking finding) --
    // Snapshot the entities so any application failure rolls the WHOLE save back (last-good — never a
    // partial load). The header is edited only on the success path. Selection is fully known now, so
    // breaking on the first application failure is safe.
    const std::vector<SaveEntity> entities_snapshot = save.entities;

    std::vector<std::string> migrated_types;
    for (const EligibleType& eligible : to_migrate)
    {
        // Migrate every entity's payload of this type through the SHARED per-payload primitive (the
        // editor's parse-time mechanism).
        for (std::size_t i = 0; i < save.entities.size(); ++i)
        {
            JsonValue* payload = find_member(save.entities[i].components, eligible.type);
            if (payload == nullptr)
                continue;
            const std::string pointer = "/entities/" + std::to_string(i) + "/components/" +
                                        escape_pointer_segment(eligible.type);
            if (!migrate::migrate_payload(set, eligible.type, eligible.saved_version, *payload,
                                          options.budget, pointer, result.diagnostics))
            {
                result.ok = false;
                break;
            }
        }
        if (!result.ok)
            break;
        if (eligible.saved_version < eligible.current)
            migrated_types.push_back(eligible.type);
    }

    if (!result.ok)
    {
        save.entities = entities_snapshot; // roll back every in-place payload mutation
        result.changed = false;
        return result;
    }

    // Re-stamp the header for every migrated type: its payloads are now at the current version.
    for (auto& [type, version] : save.component_versions)
    {
        if (std::find(migrated_types.begin(), migrated_types.end(), type) != migrated_types.end())
        {
            version = set.current_version(type);
            result.changed = true;
        }
    }

    return result;
}

} // namespace context::runtime::save
