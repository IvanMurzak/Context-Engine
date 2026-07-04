// The typed composition view over one authored scene document — see scene_model.h.

#include "context/editor/compose/scene_model.h"

#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/stable_id.h"
#include "context/editor/schema/json_access.h"
#include "context/editor/schema/kind_schema.h"

#include <set>
#include <utility>

namespace context::editor::compose
{

using serializer::JsonValue;

namespace
{

void add_diagnostic(SceneDoc& doc, std::string code, std::string message, std::string pointer,
                    bool blocking)
{
    ComposeDiagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    d.file = doc.path;
    d.pointer = std::move(pointer);
    d.blocking = blocking;
    doc.diagnostics.push_back(std::move(d));
}

// Validate + claim a collection entry's stable id. Returns the id on success; empty when the
// entry must be excluded (missing/invalid/duplicate — diagnosed here). `what` names the entry
// kind in messages ("entity" / "instance" / "root entity").
[[nodiscard]] std::string claim_id(SceneDoc& doc, const JsonValue& object,
                                   const std::string& pointer, const char* what,
                                   std::set<std::string>& claimed)
{
    const JsonValue* id = schema::find_member(object, "id");
    if (id == nullptr || id->type != JsonValue::Type::string)
    {
        add_diagnostic(doc, "compose.missing_id",
                       std::string("this ") + what +
                           " has no stable `id` — it cannot carry composed identity and is "
                           "excluded from composition (mint one: L-33 id-keyed collections)",
                       pointer, false);
        return {};
    }
    if (!is_stable_id(id->string_value))
    {
        add_diagnostic(doc, "compose.invalid_id",
                       std::string("this ") + what + "'s id `" + id->string_value +
                           "` is not a stable id (16..32 lowercase hex, >= 64 random bits — "
                           "L-33); the entry is excluded from composition",
                       pointer + "/id", false);
        return {};
    }
    if (!claimed.insert(id->string_value).second)
    {
        add_diagnostic(doc, "compose.duplicate_id",
                       "stable id `" + id->string_value +
                           "` is claimed twice in this file — intra-file ids are file-scoped "
                           "unique (L-33/R-FILE-012); the later entry is excluded",
                       pointer + "/id", true);
        return {};
    }
    return id->string_value;
}

void malformed(SceneDoc& doc, const std::string& pointer, const std::string& why)
{
    add_diagnostic(doc, "compose.override_malformed",
                   "override entry ignored: " + why, pointer, false);
}

// Parse one override entry; false when it must be excluded (diagnosed).
[[nodiscard]] bool parse_override(SceneDoc& doc, const JsonValue& entry,
                                  const std::string& pointer, OverrideEntry& out)
{
    if (entry.type != JsonValue::Type::object)
    {
        malformed(doc, pointer, "an override entry is an object");
        return false;
    }
    const JsonValue* path = schema::find_member(entry, "path");
    if (path == nullptr || path->type != JsonValue::Type::array || path->elements.empty())
    {
        malformed(doc, pointer, "`path` (a non-empty array of id strings) is required");
        return false;
    }
    std::vector<std::string> segments;
    segments.reserve(path->elements.size());
    for (const JsonValue& seg : path->elements)
    {
        const bool root_token = seg.type == JsonValue::Type::string &&
                                seg.string_value == std::string(kSceneRootId);
        if (seg.type != JsonValue::Type::string ||
            (!is_stable_id(seg.string_value) && !root_token))
        {
            malformed(doc, pointer + "/path",
                      "every id-path segment is a stable id (or the `$root` token)");
            return false;
        }
        segments.push_back(seg.string_value);
    }

    const JsonValue* field_pointer = schema::find_member(entry, "pointer");
    const JsonValue* value = schema::find_member(entry, "value");
    const JsonValue* add = schema::find_member(entry, "add");
    const JsonValue* remove = schema::find_member(entry, "remove");
    const int kinds = (field_pointer != nullptr || value != nullptr ? 1 : 0) +
                      (add != nullptr ? 1 : 0) + (remove != nullptr ? 1 : 0);
    if (kinds != 1)
    {
        malformed(doc, pointer,
                  "exactly one override kind per entry: `pointer`+`value` (field), `add` "
                  "(structural add), or `remove: true` (structural remove)");
        return false;
    }

    out.path = std::move(segments);
    out.pointer = pointer;
    if (remove != nullptr)
    {
        if (remove->type != JsonValue::Type::boolean || !remove->boolean_value)
        {
            malformed(doc, pointer + "/remove", "`remove` must be the literal true");
            return false;
        }
        out.kind = OverrideKind::remove;
        return true;
    }
    if (add != nullptr)
    {
        if (add->type != JsonValue::Type::object)
        {
            malformed(doc, pointer + "/add", "`add` must be an entity object");
            return false;
        }
        out.kind = OverrideKind::add;
        out.value = *add;
        return true;
    }
    if (field_pointer == nullptr || field_pointer->type != JsonValue::Type::string ||
        value == nullptr)
    {
        malformed(doc, pointer, "a field override pairs a string `pointer` with a `value`");
        return false;
    }
    std::vector<std::string> tokens;
    if (!parse_json_pointer(field_pointer->string_value, tokens))
    {
        malformed(doc, pointer + "/pointer", "`pointer` is not a valid non-empty JSON pointer");
        return false;
    }
    // Identity and the schema header are immutable under composition: overriding an entity's
    // `id` would fork composed identity (L-37: ids survive re-derivation and upgrade untouched).
    if (!tokens.empty() && (tokens.front() == "id" || tokens.front() == "$schema" ||
                            tokens.front() == "version"))
    {
        malformed(doc, pointer + "/pointer",
                  "`/" + tokens.front() +
                      "` is immutable under composition (L-37: stable ids and the schema header "
                      "are never overridden)");
        return false;
    }
    out.kind = OverrideKind::field;
    out.field_pointer = field_pointer->string_value;
    out.value = *value;
    return true;
}

} // namespace

bool is_scene_document(const JsonValue& root)
{
    if (root.type != JsonValue::Type::object)
        return false;
    const JsonValue* schema = schema::find_member(root, "$schema");
    return schema != nullptr && schema->type == JsonValue::Type::string &&
           schema->string_value == std::string(schema::kSceneKindId);
}

std::optional<SceneDoc> build_scene_doc(std::string_view path, const JsonValue& root)
{
    if (!is_scene_document(root))
        return std::nullopt;

    SceneDoc doc;
    doc.path = std::string(path);
    std::set<std::string> claimed; // the file-scoped id space (root + entities + instances)

    // --- scene-root entity (L-35 scene-level state) ---------------------------------------------
    if (const JsonValue* root_entity = schema::find_member(root, "root");
        root_entity != nullptr && root_entity->type == JsonValue::Type::object)
    {
        doc.root.present = true;
        doc.root.pointer = "/root";
        doc.root.value = *root_entity;
        if (const JsonValue* id = schema::find_member(*root_entity, "id"); id != nullptr)
        {
            // An explicit root id joins the file id space under the entity rules.
            doc.root.id = claim_id(doc, *root_entity, "/root", "root entity", claimed);
        }
        if (const JsonValue* composable = schema::find_member(*root_entity, "composable");
            composable != nullptr && composable->type == JsonValue::Type::boolean)
        {
            doc.root.composable = composable->boolean_value;
        }
    }

    // --- entities (id-keyed, L-33) ---------------------------------------------------------------
    if (const JsonValue* entities = schema::find_member(root, "entities");
        entities != nullptr && entities->type == JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < entities->elements.size(); ++i)
        {
            const JsonValue& e = entities->elements[i];
            const std::string pointer = "/entities/" + std::to_string(i);
            if (e.type != JsonValue::Type::object)
                continue; // shape violations are the schema validator's finding, not compose's
            std::string id = claim_id(doc, e, pointer, "entity", claimed);
            if (id.empty())
                continue;
            AuthoredEntity out;
            out.id = std::move(id);
            out.value = e;
            out.pointer = pointer;
            doc.entities.push_back(std::move(out));
        }
    }

    // --- instances (scene composition, L-35) -----------------------------------------------------
    if (const JsonValue* instances = schema::find_member(root, "instances");
        instances != nullptr && instances->type == JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < instances->elements.size(); ++i)
        {
            const JsonValue& inst = instances->elements[i];
            const std::string pointer = "/instances/" + std::to_string(i);
            if (inst.type != JsonValue::Type::object)
                continue;
            const JsonValue* scene = schema::find_member(inst, "scene");
            if (scene == nullptr || scene->type != JsonValue::Type::string ||
                scene->string_value.empty())
            {
                add_diagnostic(doc, "compose.override_malformed",
                               "instance entry ignored: `scene` (a project-relative path string) "
                               "is required",
                               pointer, false);
                continue;
            }
            std::string id = claim_id(doc, inst, pointer, "instance", claimed);
            if (id.empty())
                continue;
            SceneInstance out;
            out.id = std::move(id);
            out.scene = scene->string_value;
            out.pointer = pointer;
            doc.instances.push_back(std::move(out));
        }
    }

    // --- override entries (L-35 id-path addressing) ----------------------------------------------
    if (const JsonValue* overrides = schema::find_member(root, "overrides");
        overrides != nullptr && overrides->type == JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < overrides->elements.size(); ++i)
        {
            const std::string pointer = "/overrides/" + std::to_string(i);
            OverrideEntry entry;
            if (parse_override(doc, overrides->elements[i], pointer, entry))
                doc.overrides.push_back(std::move(entry));
        }
    }

    return doc;
}

} // namespace context::editor::compose
