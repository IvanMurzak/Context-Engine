// The live inspector feed implementation (see inspector_feed.h for the design + tolerance
// rationale). The parsers mirror builders::inspector_to_wire member-for-member; the feed tests link
// both halves and assert the round-trip.

#include "context/editor/shell/panels/inspector_feed.h"

#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/sidecar_ref.h"       // parse_hash_string — the decimal-u64 inverse
#include "context/editor/shell/panels/builtin_panels.h"  // kDerivationTopic
#include "context/editor/shell/panels/problems_feed.h"   // kDerivationSettledEvent
#include "context/editor/shell/panels/scenetree_feed.h"  // parse_hex_u64 — the ONE hex-wire parser
#include "wire_read.h"                                   // read_string / read_bool / envelope_data

#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::shell::panels
{

namespace
{

namespace serializer = context::editor::serializer;

// One wire field entry -> one model field. nullopt when the entry carries no pointer (a field the
// panel could neither label nor stage an edit against). The `value` member is the field's CANONICAL
// serialization (R-FILE-001 — the engine's one value identity); an unparseable value degrades to
// null WITH the field kept readonly, so a corrupt value is visible-but-uneditable rather than
// silently editable-as-garbage.
[[nodiscard]] std::optional<inspector::InspectorField> parse_field(const contract::Json& wire)
{
    if (!wire.is_object())
    {
        return std::nullopt;
    }
    inspector::InspectorField field;
    field.pointer = read_string(wire, "pointer");
    if (field.pointer.empty())
    {
        return std::nullopt;
    }
    field.label = read_string(wire, "label");
    field.description = read_string(wire, "description");
    field.units = read_string(wire, "units");
    field.kind = parse_widget_kind(read_string(wire, "kind"));
    field.overridden = read_bool(wire, "overridden");
    field.editable = read_bool(wire, "editable");
    // FAIL-CLOSED ACROSS BOTH MEMBERS: an unknown kind token parses to `readonly` (see
    // parse_widget_kind), and a readonly widget must not stay editable just because the wire's
    // INDEPENDENT `editable` bit said so — InspectorPanel::stage_edit gates on `editable` alone,
    // so without this clamp a future/hostile token would be visible AND editable through a widget
    // this build cannot render honestly. The real builder never emits readonly+editable
    // (inspector_builder.cpp derives editable from the kind), so round-trips are unchanged.
    if (field.kind == inspector::WidgetKind::readonly)
    {
        field.editable = false;
    }

    // Parse the canonical value straight off the wire node — as_string() is a reference, so the
    // (arbitrarily large for json-kind fields) canonical serialization is never copied first.
    serializer::ParseResult parsed = serializer::parse_json(wire.at("value").as_string());
    if (parsed.ok)
    {
        field.value = std::move(parsed.root);
    }
    else
    {
        field.value = serializer::JsonValue{}; // null — and never editable as garbage:
        field.kind = inspector::WidgetKind::readonly;
        field.editable = false;
    }
    return field;
}

} // namespace

// --------------------------------------------------------------------------------- pure parsers

inspector::WidgetKind parse_widget_kind(const std::string& token)
{
    if (token == "text")
    {
        return inspector::WidgetKind::text;
    }
    if (token == "number")
    {
        return inspector::WidgetKind::number;
    }
    if (token == "toggle")
    {
        return inspector::WidgetKind::toggle;
    }
    if (token == "json")
    {
        return inspector::WidgetKind::json;
    }
    // "readonly" and every unknown future token: visible, never editable (fail-closed).
    return inspector::WidgetKind::readonly;
}

std::optional<inspector::InspectorModel> parse_inspector(const contract::Json& wire)
{
    if (!wire.is_object() || !wire.contains("present"))
    {
        return std::nullopt; // says nothing about a selection — not the same as "no selection"
    }
    inspector::InspectorModel model;
    if (!wire.at("present").as_bool())
    {
        return model; // the engaged no-selection state (has_entity == false)
    }
    model.has_entity = true;
    model.root_scene = read_string(wire, "rootScene");
    model.identity = read_string(wire, "identity");
    model.identity_hash = parse_hex_u64(read_string(wire, "identityHash"));
    model.kind_id = read_string(wire, "kindId");
    const contract::Json& id_path = wire.at("idPath"); // at() is total: null when absent
    if (id_path.is_array())
    {
        for (std::size_t i = 0; i < id_path.size(); ++i)
        {
            if (id_path.at(i).is_string())
            {
                model.id_path.push_back(id_path.at(i).as_string());
            }
        }
    }
    const contract::Json& fields = wire.at("fields");
    if (fields.is_array())
    {
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            if (std::optional<inspector::InspectorField> field = parse_field(fields.at(i)))
            {
                if (field->overridden)
                {
                    ++model.override_count;
                }
                model.fields.push_back(std::move(*field));
            }
        }
    }
    return model;
}

std::uint64_t parse_raw_hash(const std::string& text)
{
    // serializer::parse_hash_string is THE strict inverse of the daemon's decimal hash form
    // (exactly the strings std::to_string(std::uint64_t) — kernel_server's hash_string — produces);
    // any refusal degrades to 0, the model's honest "no CAS token".
    return serializer::parse_hash_string(text).value_or(0);
}

std::optional<inspector::InspectorModel> parse_inspect_reply(const contract::Json& reply,
                                                             std::uint64_t& raw_hash)
{
    // Envelope tolerance (mirrors SceneTreeFeed::apply_result): the rawHash rides the DATA level,
    // sibling of `inspector`, so resolve data first (the shared hop — wire_read.h) and read both
    // from there. Written BEFORE the model parse so a reply whose model does not resolve still
    // reports the file's token.
    const contract::Json& data = envelope_data(reply);
    raw_hash = parse_raw_hash(read_string(data, "rawHash"));

    // The `inspector` hop is this feed's own, because which key to look for is policy.
    const contract::Json& nested = data.at("inspector");
    return parse_inspector(nested.is_object() ? nested : data);
}

std::optional<std::string> inspector_widget_pointer(const std::string& node_id)
{
    constexpr std::string_view prefix = kInspectorWidgetPrefix;
    if (node_id.size() <= prefix.size() || !std::string_view(node_id).starts_with(prefix))
    {
        return std::nullopt;
    }
    return node_id.substr(prefix.size());
}

// ------------------------------------------------------------------------------------ the feed

InspectorFeed::InspectorFeed(PanelHost& host, std::string panel_id)
    : host_(host), panel_id_(std::move(panel_id))
{
    // The commit listener is registered ONCE, here, rather than by the composition root: it is the
    // feed's OWN read-your-writes wiring (05 §7), not a policy a caller chooses. Capturing `this` is
    // safe by construction — InspectorFeed is non-copyable AND non-movable, so the address is stable
    // for the object's whole life, and the panel it registers on is a member (destroyed with it).
    panel_.add_commit_listener([this](const inspector::CommitResult& result) { on_commit(result); });
}

void InspectorFeed::bind_gateway(const inspector::OverrideWriteGateway* gateway)
{
    panel_.set_gateway(gateway);
    gateway_bound_ = gateway != nullptr;
}

void InspectorFeed::bind_checkpoint_sink(CheckpointSink sink)
{
    sink_ = std::move(sink);
}

void InspectorFeed::bind_notice_sink(NoticeSink sink)
{
    notice_sink_ = std::move(sink);
}

std::optional<undo::FieldEdit> InspectorFeed::snapshot_checkpoint() const
{
    const serializer::JsonValue* after = panel_.staged_value();
    const serializer::JsonValue* before = panel_.staged_base_value();
    if (after == nullptr || before == nullptr)
    {
        return std::nullopt; // no gesture in flight
    }
    const inspector::InspectorModel& model = panel_.model();
    if (model.root_scene.empty() || panel_.staged_pointer().empty())
    {
        return std::nullopt; // unaddressable — see the header on why this is not recorded
    }
    undo::FieldEdit edit;
    edit.root_scene = model.root_scene;
    edit.id_path = model.id_path;
    edit.pointer = panel_.staged_pointer();
    // `before` is the field's value at STAGE time — the same value the L-30 engine uses as this
    // gesture's collision base, which is exactly what an undo must restore and what a redo must find
    // the field holding. Reusing it (rather than re-reading) is what keeps the journal's no-clobber
    // guard and the commit's rebase-or-drop decision judging the same bytes.
    edit.before = *before;
    edit.after = *after;
    return edit;
}

void InspectorFeed::on_commit(const inspector::CommitResult& result)
{
    if (result.status == inspector::CommitResult::Status::none)
    {
        return; // nothing was staged / no gateway — not a resolved gesture, so not an observation
    }
    ++commits_observed_;
    last_commit_ = result;
    if (result.status == inspector::CommitResult::Status::dropped)
    {
        // L-30: a concurrent writer moved THIS field under the in-flight gesture, so the write was
        // refused rather than clobbering it. Counted, kept in `last_commit_`, and reported to stderr
        // — the stderr line is the LAST-RESORT channel (a headless run, a build with no renderer),
        // not the human-visible one. That is the sink below.
        ++drops_observed_;
        std::fprintf(stderr, "context_editor: %s\n", result.message.c_str());
    }
    if (!result.ok())
    {
        // M9 e09b-3 — THE LOUD SURFACE. A refused write (an L-30 drop OR a write-path error) is a
        // moment design 10 makes non-negotiably loud, so it goes out to the notice sink here, at the
        // ONE place that knows the outcome. Deliberately BEFORE the early return the applied path
        // never reaches: `ok()` is false for exactly `dropped` and `error`, which is exactly the set
        // the human must be told about.
        if (notice_sink_)
        {
            ++notices_sent_;
            notice_sink_(result);
        }
        // e09e-2: a refusal wrote nothing, so there is no read-your-writes barrier to honour here —
        // but a `derivation.settled` that arrived WHILE this gesture was staged was deferred rather
        // than served, and it is still owed. For a DROP that settle is precisely the concurrent write
        // that caused the drop, so the panel is currently rendering a value that is no longer on
        // disk: without this release the human is told "re-make your edit against what is there now"
        // while still being shown what WAS there, which makes the instruction unactionable. The
        // flush's own `has_staged_edit()` guard is what keeps an `error` — which KEEPS the gesture —
        // deferred instead.
        (void)flush_deferred_refresh();
        return;
    }
    // READ-YOUR-WRITES (05 §7). The panel must observe its own commit, so the next
    // `pump_panel_feeds` re-reads `editor.inspect` and the field comes back with its new value and
    // `overridden:true`. Without this the panel would wait for the next selection change or
    // `derivation.settled` — i.e. it would render a value it had already successfully written as if
    // it had not.
    //
    // This re-read SUBSUMES any deferred settle refresh: it re-requests the same identity, which is
    // exactly what the deferral was owed, so the flag is cleared rather than flushed (flushing would
    // arm the same fetch twice and double-count `rereads_armed`).
    refresh_deferred_ = false;
    (void)request_refresh();
}

bool InspectorFeed::flush_deferred_refresh()
{
    if (!refresh_deferred_ || panel_.has_staged_edit())
    {
        return false; // nothing owed, or the gesture it protects is still in flight
    }
    refresh_deferred_ = false;
    return request_refresh();
}

bool InspectorFeed::apply_event(const std::string& topic, const contract::Json& payload)
{
    if (topic != kDerivationTopic || read_string(payload, "event") != kDerivationSettledEvent)
    {
        return false; // another feed's topic, or another `derivation` event — not ours to interpret
    }
    // Counted BEFORE the guard below, deliberately: it is what distinguishes "recognized the settle
    // and withheld the re-read" from "never saw the settle at all" (inspector_feed.h § events_applied).
    ++events_applied_;
    if (panel_.has_staged_edit())
    {
        // ⚠⚠ THE L-30 GUARD. Serving the re-read here would let `apply_result` -> `set_model` discard
        // the human's in-flight edit AND adopt the concurrent writer's post-write raw hash as this
        // gesture's CAS base — so the commit would guard against the very state it is racing, find no
        // mismatch, and overwrite it. Nothing would report an error. Defer; `flush_deferred_refresh`
        // releases it the moment the gesture is genuinely gone.
        refresh_deferred_ = true;
        return false;
    }
    return request_refresh();
}

bool InspectorFeed::request_refresh()
{
    const std::string& identity = panel_.model().identity;
    if (identity.empty())
    {
        return false; // nothing inspected — an ordinary state, not a failure
    }
    // Through the NAMED seam, not a second `pending_ = ...`: `request` is the one place that
    // documents the replace-a-pending-fetch rule. (It replaces unconditionally, so a selection that
    // moved between the gesture and this call is re-armed onto the OLD identity and its fetch waits
    // for the next pump-triggering change — narrow, and noted in the PR body.)
    request(identity);
    ++rereads_armed_;
    return true;
}

void InspectorFeed::request(const std::string& identity)
{
    pending_ = identity;
}

void InspectorFeed::request_clear()
{
    pending_.reset();
    // e09e-2: a deferred settle refresh was owed to an ENTITY that is no longer inspected, so it dies
    // with the selection. Leaving it set would arm a re-read of whatever gets selected NEXT the first
    // time a gesture there resolves — harmless in effect, dishonest in bookkeeping.
    refresh_deferred_ = false;
    panel_.clear();
    host_.touch(panel_id_);
}

bool InspectorFeed::apply_result(const contract::Json& reply)
{
    std::uint64_t raw_hash = 0;
    std::optional<inspector::InspectorModel> model = parse_inspect_reply(reply, raw_hash);
    if (!model.has_value())
    {
        return false;
    }
    panel_.set_model(std::move(*model), raw_hash);
    ++results_applied_;
    // e09e-2: the panel just adopted fresh state, which IS what a deferred settle refresh was owed —
    // so nothing is outstanding any more, whichever caller armed this fetch (a selection change, a
    // read-your-writes re-request, or the deferral's own release).
    refresh_deferred_ = false;
    host_.touch(panel_id_);
    return true;
}

PanelProvider InspectorFeed::make_provider()
{
    PanelProvider provider;
    provider.build = [this] { return panel_.build_panel(); };
    provider.invoke = [this](const std::string& command_id, const contract::Json& params)
    {
        if (command_id != inspector::InspectorPanel::kEditCommand)
        {
            return false;
        }
        const std::optional<std::string> pointer =
            inspector_widget_pointer(read_string(params, "nodeId"));
        if (!pointer.has_value())
        {
            return false;
        }
        // The edit VALUE arrives as a JSON literal string (`"1.5"`, `"\"name\""`, `"true"`). A
        // dispatch with no parseable value is DECLINED — there is nothing honest to stage, and
        // stage_edit's own field/editable checks still apply to the rest.
        serializer::ParseResult value = serializer::parse_json(params.at("value").as_string());
        if (!value.ok)
        {
            return false;
        }
        return panel_.stage_edit(*pointer, std::move(value.root));
    };

    // The L-20 gesture, bound ONLY when a write path exists (inspector_feed.h): with no gateway the
    // manifest reports `gestures:false` and the hydration runtime never installs its pointer
    // handlers (`hydration.ts` #bindGestures), so the renderer cannot send a verb this build could
    // only swallow.
    if (gateway_bound_)
    {
        provider.gesture = [this](GestureVerb verb, const contract::Json& params)
        {
            switch (verb)
            {
            case GestureVerb::begin:
            case GestureVerb::extend:
                // A text/number widget has no continuous geometry — the VALUE arrives through the
                // `inspector.edit` command above, and these two only bracket it. Accepted (so the
                // renderer's gesture state machine stays in step) but only for a node that really is
                // an inspector widget: a gesture on a status row is honestly `dispatched:false`.
                return inspector_widget_pointer(read_string(params, "nodeId")).has_value();
            case GestureVerb::commit:
            {
                // GESTURE END = COMMIT (L-20). Everything downstream — CAS on the model's base hash,
                // the L-30 rebase-or-drop engine, the wire write — is InspectorPanel::commit's, and
                // the outcome reaches `on_commit` through the listener registered in the ctor.
                //
                // e09c: snapshot the reversible before/after pair FIRST. `commit()` consumes the
                // staged gesture, so the commit LISTENER — which fires inside that call — can no
                // longer see either value; the checkpoint has to be taken here, on the near side.
                const std::optional<undo::FieldEdit> checkpoint = snapshot_checkpoint();
                const inspector::CommitResult result = panel_.commit();
                if (result.status == inspector::CommitResult::Status::none)
                {
                    return false; // nothing staged: an ordinary outcome, not a protocol fault
                }
                // Journal ONLY a write that actually landed (applied / rebased). A loud L-30 drop
                // and a write-path error both left the file untouched, so there is nothing to
                // revert — see inspector_feed.h § CheckpointSink.
                if (result.ok() && checkpoint.has_value() && sink_)
                {
                    sink_(*checkpoint);
                    ++checkpoints_sent_;
                }
                host_.touch(panel_id_);
                return true;
            }
            case GestureVerb::cancel:
                if (!panel_.has_staged_edit())
                {
                    return false;
                }
                panel_.discard_edit();
                // e09e-2: the gesture is over and `discard_edit` fires NO commit listener, so this is
                // the only place a settle deferred during an abandoned gesture can be released. Miss
                // it and the panel silently keeps rendering pre-write state until the next selection
                // change — the exact staleness this task exists to remove.
                (void)flush_deferred_refresh();
                host_.touch(panel_id_);
                return true;
            }
            return false; // unreachable: parse_gesture_verb refuses anything outside the closed set
        };
    }
    return provider;
}

} // namespace context::editor::shell::panels
