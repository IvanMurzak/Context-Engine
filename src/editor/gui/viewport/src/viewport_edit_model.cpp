// The in-context viewport override-editing model — see viewport_edit_model.h. Flattens the composed
// scene for selection + provenance, runs the L-20 gizmo gesture in memory, and commits at gesture end
// through inspector::commit_override_write (the ONE L-30 engine) over the SAME compose::plan_write path
// `context set` runs.

#include "context/editor/gui/viewport/viewport_edit_model.h"

#include "context/editor/compose/json_pointer.h" // resolve_json_pointer
#include "context/editor/gui/panels/builders/scene_tree_builder.h" // join_identity — the ONE key encoding

#include <cmath>
#include <sstream>
#include <utility>

namespace context::editor::gui::viewport
{

namespace
{

using serializer::JsonValue;

using panels::builders::join_identity; // the exported stable-selection-key join

// The value of a string member `key` on an object, or empty when absent / not a string.
[[nodiscard]] std::string string_member(const JsonValue& value, const char* key)
{
    if (value.type != JsonValue::Type::object)
    {
        return {};
    }
    for (const serializer::JsonMember& m : value.members)
    {
        if (m.key == key && m.value.type == JsonValue::Type::string)
        {
            return m.value.string_value;
        }
    }
    return {};
}

// Read a JSON scalar as a double, whatever its numeric representation.
[[nodiscard]] double as_double(const JsonValue& v) noexcept
{
    switch (v.type)
    {
    case JsonValue::Type::integer:
        return static_cast<double>(v.int_value);
    case JsonValue::Type::unsigned_integer:
        return static_cast<double>(v.uint_value);
    case JsonValue::Type::number:
        return v.number_value;
    default:
        return 0.0;
    }
}

// Build a scalar JSON value from a double, keeping an integral result integer-typed (so a translated
// position serializes exactly like the authored integer literal — byte-parity with `context set`).
[[nodiscard]] JsonValue make_scalar(double v)
{
    JsonValue out;
    const double rounded = std::nearbyint(v);
    // Fits losslessly in i64 AND is exactly integral -> emit an integer literal.
    if (rounded == v && v >= -9.007199254740992e15 && v <= 9.007199254740992e15)
    {
        out.type = JsonValue::Type::integer;
        out.int_value = static_cast<std::int64_t>(rounded);
    }
    else
    {
        out.type = JsonValue::Type::number;
        out.number_value = v;
    }
    return out;
}

} // namespace

bool ViewportEditModel::open(const compose::SceneResolver& resolver, std::string root_scene,
                             const compose::ComposeLimits& limits)
{
    loaded_ = false;
    clear_selection();
    root_scene_ = std::move(root_scene);
    refs_.clear();

    scene_ = compose::flatten(root_scene_, resolver, limits);
    if (scene_.entities.empty())
    {
        return false; // the root scene did not resolve to any composed entity
    }

    refs_.reserve(scene_.entities.size());
    for (const compose::ComposedEntity& e : scene_.entities)
    {
        ComposedEntityRef ref;
        ref.identity = join_identity(e.id_path);
        ref.identity_hash = e.identity_hash;
        ref.id_path = e.id_path;
        ref.instanced = e.id_path.size() > 1;
        ref.name = string_member(e.value, "name");
        refs_.push_back(std::move(ref));
    }
    loaded_ = true;
    return true;
}

bool ViewportEditModel::select(const std::string& identity)
{
    cancel_gesture();
    for (std::size_t i = 0; i < refs_.size(); ++i)
    {
        if (refs_[i].identity == identity)
        {
            has_selection_ = true;
            selected_index_ = i;
            selection_identity_ = identity;
            return true;
        }
    }
    return false;
}

void ViewportEditModel::clear_selection()
{
    cancel_gesture();
    has_selection_ = false;
    selected_index_ = 0;
    selection_identity_.clear();
}

const std::vector<std::string>& ViewportEditModel::selected_id_path() const
{
    static const std::vector<std::string> kEmpty;
    return has_selection_ ? refs_[selected_index_].id_path : kEmpty;
}

const compose::ComposedEntity* ViewportEditModel::selected_entity() const
{
    if (!has_selection_ || selected_index_ >= scene_.entities.size())
    {
        return nullptr;
    }
    return &scene_.entities[selected_index_];
}

const serializer::JsonValue* ViewportEditModel::value_at(const std::string& pointer) const
{
    const compose::ComposedEntity* entity = selected_entity();
    if (entity == nullptr)
    {
        return nullptr;
    }
    return compose::resolve_json_pointer(entity->value, pointer);
}

bool ViewportEditModel::overridden_at(const std::string& pointer) const
{
    const compose::ComposedEntity* entity = selected_entity();
    if (entity == nullptr)
    {
        return false;
    }
    for (const compose::FieldProvenance& fp : entity->field_provenance)
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

std::vector<compose::ProvenanceEntry> ViewportEditModel::provenance(const std::string& pointer) const
{
    const compose::ComposedEntity* entity = selected_entity();
    if (entity == nullptr)
    {
        return {};
    }
    return compose::provenance_for(*entity, pointer);
}

std::string ViewportEditModel::provenance_json(const std::string& pointer) const
{
    return compose::provenance_json(provenance(pointer));
}

const char* ViewportEditModel::gizmo_pointer() const noexcept
{
    switch (gizmo_)
    {
    case Gizmo::move:
        return kPositionPointer;
    case Gizmo::rotate:
        return kRotationPointer;
    case Gizmo::scale:
        return kScalePointer;
    }
    return kPositionPointer;
}

bool ViewportEditModel::begin_gesture(const inspector::OverrideWriteGateway& gateway)
{
    cancel_gesture();
    const compose::ComposedEntity* entity = selected_entity();
    if (entity == nullptr)
    {
        return false;
    }
    const std::string pointer = gizmo_pointer();
    const serializer::JsonValue* current = compose::resolve_json_pointer(entity->value, pointer);
    if (current == nullptr)
    {
        return false; // the gizmo's field is absent — use a property edit to introduce it instead
    }

    gesture_pointer_ = pointer;
    collision_base_ = *current;
    pending_ = *current;
    // The CAS token guards the outermost (root) scene file — the L-30 gesture-conflict path. Explicit
    // retargets (template / at-instance) commit single-writer without a CAS guard (0 = no --if-match,
    // exactly like `context set` without --if-match), so a retarget never CAS-mismatches the ROOT hash.
    base_raw_hash_ = (edit_target_ == EditTarget::outermost)
                         ? gateway.read(root_scene_, entity->id_path, pointer).raw_hash
                         : 0;
    gesture_active_ = true;
    return true;
}

void ViewportEditModel::translate(double dx, double dy, double dz)
{
    if (!gesture_active_ || pending_.type != JsonValue::Type::array || pending_.elements.size() != 3)
    {
        return;
    }
    const double deltas[3] = {dx, dy, dz};
    for (std::size_t i = 0; i < 3; ++i)
    {
        pending_.elements[i] = make_scalar(as_double(pending_.elements[i]) + deltas[i]);
    }
}

void ViewportEditModel::set_pending_value(serializer::JsonValue value)
{
    if (!gesture_active_)
    {
        return;
    }
    pending_ = std::move(value);
}

void ViewportEditModel::cancel_gesture()
{
    gesture_active_ = false;
    gesture_pointer_.clear();
    collision_base_ = serializer::JsonValue{};
    pending_ = serializer::JsonValue{};
    base_raw_hash_ = 0;
}

inspector::CommitResult ViewportEditModel::commit_gesture(const inspector::OverrideWriteGateway& gateway)
{
    inspector::CommitResult result;
    const compose::ComposedEntity* entity = selected_entity();
    if (!gesture_active_ || entity == nullptr)
    {
        return result; // Status::none
    }

    // The boundary-clean envelope (M9 e05d3): the gateway converts it to the compose::WriteRequest
    // the write path consumes (builders::to_write_request), so the seam itself carries no kernel type.
    inspector::OverrideWriteRequest request;
    request.root_scene = root_scene_;
    request.id_path = entity->id_path;
    request.pointer = gesture_pointer_;
    request.value = pending_;
    switch (edit_target_)
    {
    case EditTarget::outermost:
        request.target = inspector::OverrideWriteTarget::outermost;
        break;
    case EditTarget::edit_template:
        request.target = inspector::OverrideWriteTarget::defining_template;
        break;
    case EditTarget::at_instance:
        request.target = inspector::OverrideWriteTarget::at_instance;
        request.at_instance = at_instance_;
        break;
    }

    result = inspector::commit_override_write(gateway, request, root_scene_, entity->id_path,
                                              gesture_pointer_, collision_base_, base_raw_hash_);

    // Consume the gesture on any decided outcome (applied / rebased / dropped); keep it only when the
    // write path REFUSED the request (Status::error) so the caller can adjust + retry (mirrors the
    // inspector panel's commit discipline).
    if (result.status != inspector::CommitResult::Status::error)
    {
        cancel_gesture();
    }
    return result;
}

std::string ViewportEditModel::status_text() const
{
    std::ostringstream out;
    out << "Viewport edit - ";
    if (!has_selection_)
    {
        out << "no selection";
        return out.str();
    }
    const char* gizmo = gizmo_ == Gizmo::move ? "move" : (gizmo_ == Gizmo::rotate ? "rotate" : "scale");
    const char* target = edit_target_ == EditTarget::outermost
                             ? "outermost"
                             : (edit_target_ == EditTarget::edit_template ? "template" : "at-instance");
    out << (refs_[selected_index_].name.empty() ? selection_identity_ : refs_[selected_index_].name)
        << " - gizmo " << gizmo << " - target " << target << " - "
        << (gesture_active_ ? "editing" : "idle");
    return out.str();
}

} // namespace context::editor::gui::viewport
