// Inspector view-model builder + write-request construction. The schema-intersection logic moved
// VERBATIM from context_gui_panel_inspector (M9 e05d3) so the panel library stays boundary-clean;
// to_write_request is the ONE clean->compose conversion every kernel-side gateway routes through.

#include "context/editor/gui/panels/builders/inspector_builder.h"

#include "context/editor/compose/json_pointer.h"
#include "context/editor/serializer/json_parse.h"

#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::panels::builders
{

namespace
{

namespace serializer = context::editor::serializer;

using inspector::InspectorField;
using inspector::InspectorModel;
using inspector::WidgetKind;
using serializer::JsonValue;

// The entity-item field prefix in the scene kind's introspection index: fields under an entity are
// published as "/entities/[]/<...>" (collect_fields uses "/[]" for array items). A field is an
// entity field iff its data pointer is strictly under this prefix.
constexpr const char* kEntityFieldPrefix = "/entities/[]";

// The id-path joined with '/', the stable selection key (mirrors scene_tree_builder's join_identity).
[[nodiscard]] std::string join_identity(const std::vector<std::string>& id_path)
{
    std::string out;
    for (std::size_t i = 0; i < id_path.size(); ++i)
    {
        if (i != 0)
        {
            out += '/';
        }
        out += id_path[i];
    }
    return out;
}

// The value of a string member `key` on an object, or nullptr when absent / not a string.
[[nodiscard]] const std::string* string_member(const JsonValue& value, const char* key)
{
    if (value.type != JsonValue::Type::object)
    {
        return nullptr;
    }
    for (const serializer::JsonMember& m : value.members)
    {
        if (m.key == key && m.value.type == JsonValue::Type::string)
        {
            return &m.value.string_value;
        }
    }
    return nullptr;
}

// The last '/'-separated token of a JSON pointer (the field's leaf name — the accessible name).
[[nodiscard]] std::string leaf_of(const std::string& pointer)
{
    const std::size_t slash = pointer.find_last_of('/');
    return slash == std::string::npos ? pointer : pointer.substr(slash + 1);
}

// True iff an L-35 override contributor touched `pointer` inside `entity` (so the field is visibly
// overridden). A pointer with only a template contributor is not an override.
[[nodiscard]] bool field_overridden(const compose::ComposedEntity& entity, const std::string& pointer)
{
    for (const compose::FieldProvenance& fp : entity.field_provenance)
    {
        if (fp.pointer != pointer)
        {
            continue;
        }
        for (const compose::ProvenanceEntry& pe : fp.chain)
        {
            if (pe.source == compose::ProvenanceEntry::Source::override_value)
            {
                return true;
            }
        }
    }
    return false;
}

// Map an introspection field's declared `type` (+ annotations) onto the inspector widget. Returns
// false when the field is a container / annotation the inspector does not surface as an editable row
// (object containers are represented by their leaf children; `notes` is an L-32 annotation, not a
// component field).
[[nodiscard]] bool widget_for(const JsonValue& field_entry, WidgetKind& out)
{
    // A binary sidecar reference is not hand-editable in the inspector (read-only).
    if (string_member(field_entry, "sidecar") != nullptr)
    {
        out = WidgetKind::readonly;
        return true;
    }
    const std::string* type = string_member(field_entry, "type");
    if (type == nullptr)
    {
        // No declared scalar type but not a sidecar: a ref or union composite -> JSON literal edit.
        out = WidgetKind::json;
        return true;
    }
    if (*type == "notes" || *type == "object")
    {
        return false; // annotation / container -> not an editable leaf row
    }
    if (*type == "boolean")
    {
        out = WidgetKind::toggle;
    }
    else if (*type == "string")
    {
        out = WidgetKind::text;
    }
    else if (*type == "number" || *type == "integer")
    {
        out = WidgetKind::number;
    }
    else // "array" or anything composite
    {
        out = WidgetKind::json;
    }
    return true;
}

} // namespace

InspectorModel build_inspector_model(const compose::ComposedEntity& entity,
                                     const schema::KindSchema& kind_schema,
                                     const std::string& root_scene)
{
    InspectorModel model;
    model.root_scene = root_scene;
    model.id_path = entity.id_path;
    model.identity = join_identity(entity.id_path);
    model.identity_hash = entity.identity_hash;
    model.kind_id = kind_schema.id;
    model.has_entity = true;

    // The R-CLI-005 introspection surface for the kind: {"fields":[{pointer,type,units,...}], ...}.
    // Deriving the inspector fields from this (not a hand-walk of the schema doc) keeps the inspector
    // in lockstep with the ONE published introspection view (schema::introspection_json).
    const std::string introspection = schema::introspection_json(kind_schema);
    serializer::ParseResult parsed = serializer::parse_json(introspection);
    if (!parsed.ok || parsed.root.type != JsonValue::Type::object)
    {
        return model; // unreachable: introspection_json emits canonical, re-parsable JSON
    }

    const JsonValue* fields = nullptr;
    for (const serializer::JsonMember& m : parsed.root.members)
    {
        if (m.key == "fields" && m.value.type == JsonValue::Type::array)
        {
            fields = &m.value;
            break;
        }
    }
    if (fields == nullptr)
    {
        return model;
    }

    const std::string prefix = kEntityFieldPrefix;
    for (const JsonValue& entry : fields->elements)
    {
        const std::string* pointer = string_member(entry, "pointer");
        if (pointer == nullptr)
        {
            continue;
        }
        // Only the entity's OWN fields: strictly under "/entities/[]/".
        if (pointer->size() <= prefix.size() || pointer->compare(0, prefix.size(), prefix) != 0 ||
            (*pointer)[prefix.size()] != '/')
        {
            continue;
        }
        const std::string rel = pointer->substr(prefix.size()); // entity-relative pointer

        // The immutable identity fields survive re-derivation and are never written through
        // composition (L-37); the inspector never offers them as editable rows.
        if (compose::is_immutable_pointer(rel))
        {
            continue;
        }

        WidgetKind kind = WidgetKind::text;
        if (!widget_for(entry, kind))
        {
            continue; // a container / annotation, not an editable leaf
        }

        // Only surface fields the entity ACTUALLY carries (its present components).
        const JsonValue* current = compose::resolve_json_pointer(entity.value, rel);
        if (current == nullptr)
        {
            continue;
        }

        InspectorField field;
        field.pointer = rel;
        field.label = leaf_of(rel);
        if (const std::string* description = string_member(entry, "description"))
        {
            field.description = *description;
        }
        if (const std::string* units = string_member(entry, "units"))
        {
            field.units = *units;
        }
        field.kind = kind;
        field.value = *current;
        field.overridden = field_overridden(entity, rel);
        field.editable = kind != WidgetKind::readonly;
        if (field.overridden)
        {
            ++model.override_count;
        }
        model.fields.push_back(std::move(field));
    }

    return model;
}

compose::WriteRequest override_write_request(const InspectorModel& model, const std::string& pointer,
                                             serializer::JsonValue value)
{
    compose::WriteRequest request;
    request.root_scene = model.root_scene;
    request.id_path = model.id_path;
    request.pointer = pointer;
    request.value = std::move(value);
    request.target = compose::WriteTarget::outermost; // L-35: the outermost instancing scene wins
    return request;
}

compose::WriteRequest to_write_request(const inspector::OverrideWriteRequest& request)
{
    compose::WriteRequest out;
    out.root_scene = request.root_scene;
    out.id_path = request.id_path;
    out.pointer = request.pointer;
    out.value = request.value;
    switch (request.target)
    {
    case inspector::OverrideWriteTarget::outermost:
        out.target = compose::WriteTarget::outermost;
        break;
    case inspector::OverrideWriteTarget::defining_template:
        out.target = compose::WriteTarget::defining_template;
        break;
    case inspector::OverrideWriteTarget::at_instance:
        out.target = compose::WriteTarget::at_instance;
        break;
    }
    out.at_instance = request.at_instance;
    return out;
}

} // namespace context::editor::gui::panels::builders
