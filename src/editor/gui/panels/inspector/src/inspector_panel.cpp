// Inspector panel: schema-driven editable widget tree + the L-20 gesture-end commit through the
// `context set` override-write path, with the L-30 rebase-or-drop policy under a concurrent writer.

#include "context/editor/gui/panels/inspector/inspector_panel.h"

#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/uitree/node.h"

#include "context/editor/serializer/canonical.h"

#include <sstream>
#include <string>
#include <utility>

namespace context::editor::gui::panels::inspector
{

namespace
{

namespace serializer = context::editor::serializer;

using serializer::JsonValue;

// The bounded rebase retry budget: under a burst of concurrent UNRELATED writes each rebase raced a
// further write; after this many rebase races we drop loudly rather than spin (L-30 never overwrites,
// so a bounded give-up is safe — the user re-applies). Small + deterministic for the test harness.
constexpr int kMaxRebaseRetries = 8;

// Two JSON values are equal iff their canonical serializations match — the engine's ONE notion of
// value identity (R-FILE-001), mirroring compose_write.cpp's canonical_equal.
[[nodiscard]] bool canonical_equal(const JsonValue& a, const JsonValue& b)
{
    std::string sa;
    std::string sb;
    const bool oka = serializer::serialize_canonical(a, sa);
    const bool okb = serializer::serialize_canonical(b, sb);
    return oka == okb && sa == sb;
}

// Render a field value for display in a widget's visible text. Scalars render plainly; composites
// render as canonical JSON (the exact literal `context set` would accept). Deterministic.
[[nodiscard]] std::string render_value(const JsonValue& value)
{
    switch (value.type)
    {
    case JsonValue::Type::string:
        return value.string_value;
    case JsonValue::Type::boolean:
        return value.boolean_value ? "true" : "false";
    case JsonValue::Type::integer:
        return std::to_string(value.int_value);
    case JsonValue::Type::unsigned_integer:
        return std::to_string(value.uint_value);
    case JsonValue::Type::null_value:
        return "null";
    default:
        break;
    }
    std::string out;
    if (!serializer::serialize_canonical(value, out))
    {
        out = "null"; // unreachable: a composed value carries no non-finite number
    }
    return out;
}

// The uitree role for a field's widget.
[[nodiscard]] uitree::Role role_for(WidgetKind kind)
{
    switch (kind)
    {
    case WidgetKind::toggle:
        return uitree::Role::checkbox;
    case WidgetKind::text:
    case WidgetKind::number:
    case WidgetKind::json:
        return uitree::Role::textbox;
    case WidgetKind::readonly:
        return uitree::Role::text;
    }
    return uitree::Role::textbox;
}

// The visible label text for a field row: its pointer + description + units + override marker, so a
// sighted AND an assistive-tech user sees the field's meaning and whether it is overridden (L-35).
[[nodiscard]] std::string field_caption(const InspectorField& field)
{
    std::string caption = field.pointer;
    if (!field.description.empty())
    {
        caption += " - " + field.description;
    }
    if (!field.units.empty())
    {
        caption += " (" + field.units + ")";
    }
    if (field.overridden)
    {
        caption += " (overridden)";
    }
    return caption;
}

} // namespace

CommitResult commit_override_write(const OverrideWriteGateway& gateway,
                                   const OverrideWriteRequest& request, const std::string& root_scene,
                                   const std::vector<std::string>& id_path, const std::string& pointer,
                                   const serializer::JsonValue& collision_base,
                                   std::uint64_t base_raw_hash)
{
    CommitResult result;
    result.pointer = pointer;

    // First attempt: CAS-guarded on the caller's snapshot base hash.
    const WriteAttempt attempt = gateway.attempt(request, base_raw_hash);
    if (attempt.applied)
    {
        result.status = CommitResult::Status::applied;
        result.file = attempt.file;
        result.written_pointer = attempt.pointer;
        result.raw_hash = attempt.raw_hash;
        return result;
    }
    if (!attempt.cas_mismatch)
    {
        // The write path refused the request (e.g. compose.write_target_not_found / immutable) — not a
        // concurrency event; surface it as an error (the caller keeps its edit for the caller to fix).
        result.status = CommitResult::Status::error;
        result.code = attempt.code;
        result.message = attempt.message;
        result.raw_hash = attempt.raw_hash;
        return result;
    }

    // --- L-30 rebase-or-drop: a concurrent writer advanced the target file --------------------------
    std::uint64_t last_seen_hash = attempt.raw_hash; // the file's current CAS token, refreshed per read
    for (int retry = 0; retry < kMaxRebaseRetries; ++retry)
    {
        const FieldState current = gateway.read(root_scene, id_path, pointer);
        last_seen_hash = current.raw_hash;
        if (!canonical_equal(current.value, collision_base))
        {
            // The external change TOUCHED this field path -> drop loudly, never a silent overwrite.
            result.status = CommitResult::Status::dropped;
            result.code = "cas.mismatch";
            result.message = "the field `" + pointer +
                             "` changed under your in-flight edit (a concurrent writer); the gesture "
                             "was dropped, not overwritten (L-30 rebase-or-drop) — re-open the field "
                             "and re-apply against the current value";
            result.raw_hash = current.raw_hash;
            return result;
        }

        // This field path is UNTOUCHED by the external change -> rebase onto the new state and retry.
        const WriteAttempt rebased = gateway.attempt(request, current.raw_hash);
        if (rebased.applied)
        {
            result.status = CommitResult::Status::rebased;
            result.file = rebased.file;
            result.written_pointer = rebased.pointer;
            result.raw_hash = rebased.raw_hash;
            return result;
        }
        if (!rebased.cas_mismatch)
        {
            result.status = CommitResult::Status::error;
            result.code = rebased.code;
            result.message = rebased.message;
            result.raw_hash = rebased.raw_hash;
            return result;
        }
        // Another concurrent writer raced the rebase -> loop and re-read.
    }

    // Exhausted the rebase budget under a sustained concurrent-write burst -> drop loudly.
    result.status = CommitResult::Status::dropped;
    result.code = "cas.mismatch";
    result.message = "concurrent writers kept advancing the file; the gesture was dropped after " +
                     std::to_string(kMaxRebaseRetries) + " rebase attempts (L-30) — re-apply it";
    result.raw_hash = last_seen_hash;
    return result;
}

void InspectorPanel::set_model(InspectorModel model, std::uint64_t base_raw_hash)
{
    model_ = std::move(model);
    base_raw_hash_ = base_raw_hash;
    staged_.reset();
    staged_pointer_.clear();
}

void InspectorPanel::clear()
{
    model_ = InspectorModel{};
    base_raw_hash_ = 0;
    staged_.reset();
    staged_pointer_.clear();
}

bool InspectorPanel::stage_edit(const std::string& pointer, serializer::JsonValue new_value)
{
    const InspectorField* field = find_field(model_, pointer);
    if (field == nullptr || !field->editable)
    {
        return false;
    }
    staged_ = StagedEdit{pointer, std::move(new_value), field->value};
    staged_pointer_ = pointer;
    return true;
}

void InspectorPanel::discard_edit()
{
    staged_.reset();
    staged_pointer_.clear();
}

CommitResult InspectorPanel::commit()
{
    if (!staged_.has_value() || gateway_ == nullptr)
    {
        CommitResult result;
        result.status = CommitResult::Status::none;
        last_result_ = result;
        return result;
    }

    const StagedEdit edit = *staged_;
    // The boundary-clean envelope (M9 e05d3): the L-35 outermost-scene default, addressed by the
    // model's id-path — exactly what builders::override_write_request constructs kernel-side; the
    // gateway converts (builders::to_write_request) or maps it onto the wire (e09).
    OverrideWriteRequest request;
    request.root_scene = model_.root_scene;
    request.id_path = model_.id_path;
    request.pointer = edit.pointer;
    request.value = edit.new_value;
    request.target = OverrideWriteTarget::outermost;

    // Route the gesture through the ONE L-20/L-30 commit engine (shared with the session undo/redo
    // replay): CAS-guarded on the snapshot base hash, rebase-or-drop under a concurrent writer.
    const CommitResult result = commit_override_write(*gateway_, request, model_.root_scene,
                                                      model_.id_path, edit.pointer, edit.base_value,
                                                      base_raw_hash_);

    switch (result.status)
    {
    case CommitResult::Status::applied:
    case CommitResult::Status::rebased:
    case CommitResult::Status::dropped:
        // The gesture is resolved (landed or loudly dropped, L-30) — consume it and adopt the file's
        // new/current CAS token so the next gesture guards on live state.
        base_raw_hash_ = result.raw_hash;
        staged_.reset();
        staged_pointer_.clear();
        break;
    case CommitResult::Status::error:
        // A write-path refusal (not a concurrency event) — keep the staged gesture for the caller to fix.
    case CommitResult::Status::none:
        break;
    }

    last_result_ = result;
    notify(result);
    return result;
}

void InspectorPanel::add_commit_listener(CommitListener listener)
{
    listeners_.push_back(std::move(listener));
}

void InspectorPanel::notify(const CommitResult& result) const
{
    for (const CommitListener& listener : listeners_)
    {
        if (listener)
        {
            listener(result);
        }
    }
}

uitree::Panel InspectorPanel::build_panel() const
{
    using uitree::Role;
    using uitree::UiNode;

    uitree::Panel panel("inspector", "Inspector");

    UiNode root(Role::region, "inspector.panel");
    root.set_label("Inspector");
    root.add_child(UiNode(Role::heading, "inspector.heading")
                       .set_label("Inspector")
                       .set_text("Inspector"));

    if (!model_.has_entity)
    {
        root.add_child(UiNode(Role::status, "inspector.status")
                           .set_label("Inspector status")
                           .set_text("No entity selected"));
        panel.set_root(std::move(root));
        return panel;
    }

    // Whether the panel exposes an editable command at all (an entity with only readonly fields — or
    // no fields — exposes none, so no unreachable-command a11y violation).
    bool has_editable = false;
    for (const InspectorField& field : model_.fields)
    {
        if (field.editable)
        {
            has_editable = true;
            break;
        }
    }
    if (has_editable)
    {
        panel.add_command(kEditCommand, "Edit field");
    }

    std::ostringstream status;
    status << model_.identity << " - " << model_.kind_id << " - " << model_.fields.size()
           << " fields - " << model_.override_count << " overridden";
    root.add_child(UiNode(Role::status, "inspector.status")
                       .set_label("Inspector status")
                       .set_text(status.str()));

    UiNode list(Role::list, "inspector.fields");
    list.set_label("Component fields");
    for (const InspectorField& field : model_.fields)
    {
        UiNode row(Role::group, "inspector.field." + field.pointer);
        row.add_child(UiNode(Role::text, "inspector.label." + field.pointer)
                          .set_text(field_caption(field)));

        UiNode widget(role_for(field.kind), "inspector.widget." + field.pointer);
        widget.set_label(field.pointer); // the accessible name (stable, greppable)
        widget.set_text(render_value(field.value));
        if (field.editable)
        {
            widget.set_focusable(true);
            widget.set_command(kEditCommand);
        }
        row.add_child(std::move(widget));
        list.add_child(std::move(row));
    }
    root.add_child(std::move(list));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::panels::inspector
