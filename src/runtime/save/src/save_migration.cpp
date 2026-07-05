// Minimal save-migration runner implementation (see save_migration.h).

#include "context/runtime/save/save_migration.h"

#include <algorithm>
#include <cstddef>
#include <string>
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
                          "/entities/" + std::to_string(i) + "/components/" + comp.key);
                result.ok = false;
            }
        }
    }
    if (!result.ok)
        return result; // nothing applied yet

    // All-or-nothing per save: snapshot the entities so any blocking finding rolls the WHOLE save
    // back (last-good — never a partial load). The header is edited only on the success path.
    const std::vector<SaveEntity> entities_snapshot = save.entities;

    std::vector<std::string> migrated_types;
    for (const auto& [type, saved_version] : save.component_versions)
    {
        const std::int64_t current = set.current_version(type);
        if (current == 0)
        {
            push_diag(result.diagnostics, "save.unknown_component",
                      "the save carries component \"" + type +
                          "\" which this build's compiled component set does not include "
                          "(R-DATA-005)",
                      "/componentVersions/" + type);
            result.ok = false;
            break;
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
                      "/componentVersions/" + type);
            result.ok = false;
            break;
        }
        if (saved_version < current && (current - saved_version) > options.back_compat_scope)
        {
            push_diag(result.diagnostics, "save.back_compat_exceeded",
                      "\"" + type + "\" is stamped version " + std::to_string(saved_version) +
                          " but this build is version " + std::to_string(current) +
                          ", beyond the declared " + std::to_string(options.back_compat_scope) +
                          "-version save back-compat scope (R-DATA-005)",
                      "/componentVersions/" + type);
            result.ok = false;
            break;
        }

        // saved_version == current (a no-op) or older-within-scope: migrate every entity's payload
        // of this type through the SHARED per-payload primitive (the editor's parse-time mechanism).
        for (std::size_t i = 0; i < save.entities.size(); ++i)
        {
            JsonValue* payload = find_member(save.entities[i].components, type);
            if (payload == nullptr)
                continue;
            const std::string pointer = "/entities/" + std::to_string(i) + "/components/" + type;
            if (!migrate::migrate_payload(set, type, saved_version, *payload, options.budget, pointer,
                                          result.diagnostics))
            {
                result.ok = false;
                break;
            }
        }
        if (!result.ok)
            break;
        if (saved_version < current)
            migrated_types.push_back(type);
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
