// The wire override-write gateway (see wire_override_gateway.h for the D10 / lifetime /
// read-your-writes rationale). The wire shape here is the pointer/value `edit` mode e09b-1 landed;
// every member name below is asserted against a LIVE daemon by the e09b-2 T2 drill, so a rename on
// either side reddens a ctest rather than silently writing nothing.

#include "context/editor/shell/panels/wire_override_gateway.h"

#include "context/editor/client/client.h" // the wire writes (complete type HERE only)
#include "context/editor/serializer/canonical.h"
#include "context/editor/shell/panels/inspector_feed.h" // parse_inspector / parse_raw_hash — ONE parser
#include "wire_read.h"                                  // read_string / read_bool / envelope_data

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace context::editor::shell::panels
{

namespace
{

namespace serializer = context::editor::serializer;

// The canonical byte form of a staged value, as the wire carries it: a JSON literal in a STRING
// (e09b-1's convention — `contract::Json` numbers are double-backed, and the canonical serialization
// is the engine's ONE value identity, R-FILE-001). The trailing newline `serialize_canonical` appends
// for FILES is stripped, exactly as the daemon's own `builders::inspector_to_wire` strips it when it
// serializes the READ side of this same field — the two directions must agree byte-for-byte or a
// round-tripped value would not compare equal to itself.
[[nodiscard]] bool canonical_literal(const serializer::JsonValue& value, std::string& out)
{
    std::string bytes;
    if (!serializer::serialize_canonical(value, bytes))
    {
        return false;
    }
    if (!bytes.empty() && bytes.back() == '\n')
    {
        bytes.pop_back();
    }
    out = std::move(bytes);
    return true;
}

// The `cas.mismatch` refusal's fresh on-disk state (design 05 §7). e09b-1 pinned the SHAPE and it is
// worth restating because the two `edit` verbs differ: a single `edit` carries ONE conflict object
// under `error.data.data`, while `edit-batch` carries a `conflicts` ARRAY (it refuses atomically and
// must name every file). This gateway issues single `edit`s, so it reads the object form; reaching
// for `conflicts[0]` here would silently find nothing and turn every rebase into a drop.
[[nodiscard]] std::uint64_t refusal_actual_raw_hash(const client::Client& client)
{
    const contract::Json& detail = client.last_error_data().at("data");
    return parse_raw_hash(read_string(detail, "actualRawHash"));
}

} // namespace

void WireOverrideWriteGateway::bind_client(client::Client* client) noexcept
{
    client_ = client;
}

const char* WireOverrideWriteGateway::write_target_token(
    inspector::OverrideWriteTarget target) noexcept
{
    switch (target)
    {
    case inspector::OverrideWriteTarget::outermost:
        return "outermost";
    case inspector::OverrideWriteTarget::defining_template:
        return "template";
    case inspector::OverrideWriteTarget::at_instance:
        return "at-instance";
    }
    return "outermost";
}

std::string WireOverrideWriteGateway::join_id_path(const std::vector<std::string>& id_path)
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

inspector::WriteAttempt WireOverrideWriteGateway::attempt(
    const inspector::OverrideWriteRequest& request, std::uint64_t expected_raw_hash) const
{
    inspector::WriteAttempt out;

    if (client_ == nullptr)
    {
        // No daemon to ask. NOT a CAS mismatch — reporting one would send the L-30 engine into a
        // rebase loop against a gateway that cannot read either, and NOT `applied`, which would be
        // the one unforgivable lie on this path.
        out.code = kNoDaemonCode;
        out.message = "no daemon connection is bound; the edit was not delivered and nothing was "
                      "written";
        return out;
    }

    std::string value_literal;
    if (!canonical_literal(request.value, value_literal))
    {
        // Unreachable from a composed value (the grammar cannot produce NaN/Infinity), but a write
        // path must never fall through to "send something".
        out.code = "internal.error";
        out.message = "the staged value could not be canonically serialized";
        return out;
    }

    contract::Json params = contract::Json::object();
    params.set("rootScene", contract::Json(request.root_scene));
    params.set("idPath", contract::Json(join_id_path(request.id_path)));
    params.set("pointer", contract::Json(request.pointer));
    params.set("value", contract::Json(std::move(value_literal)));
    // SENT EXPLICITLY even for the `outermost` default. The daemon refuses a token it cannot parse
    // rather than defaulting (e09b-1), so an explicit token turns a drift between these two
    // vocabularies into a named refusal; omitting it would let a future retarget silently write the
    // outermost file instead of the one the caller meant.
    params.set("target", contract::Json(std::string(write_target_token(request.target))));
    if (request.target == inspector::OverrideWriteTarget::at_instance)
    {
        params.set("atInstance", contract::Json(join_id_path(request.at_instance)));
    }
    if (expected_raw_hash != 0)
    {
        // A DECIMAL STRING: a full-range u64 exceeds 2^53 and cannot survive a JSON number
        // (kernel_server's `hash_string` discipline; `parse_hash_string` is its strict inverse).
        // A zero `expected_raw_hash` means "no guard" — exactly `context set` without --if-match.
        params.set("ifMatch", contract::Json(std::to_string(expected_raw_hash)));
    }

    ++writes_issued_;
    std::string error;
    const std::optional<contract::Json> reply = client_->call("edit", std::move(params), error);
    if (!reply.has_value())
    {
        const std::string& code = client_->last_error_code();
        if (code == "cas.mismatch")
        {
            // The L-30 trigger. The refusal already carries the fresh on-disk state, so the retry
            // token comes from it — no second read round trip (design 05 §7). `read()` below still
            // re-reads, because the L-30 decision needs the FIELD's current VALUE, which the CAS
            // payload (a file-level object) does not carry.
            ++cas_refusals_;
            out.cas_mismatch = true;
            out.code = code;
            out.message = "the target file's current bytes do not match the expected hash";
            out.raw_hash = refusal_actual_raw_hash(*client_);
            return out;
        }
        // The daemon's own code, VERBATIM (each maps to a different exit class), or `internal.error`
        // for a transport fault — the R-CLI-008 rule `Client::failure_code` states in one place.
        out.code = client_->failure_code("internal.error");
        out.message = error;
        return out;
    }

    const contract::Json& data = envelope_data(*reply);
    out.applied = true;
    // `file`, not `path`: composition decides WHERE an override lands, so the file is an OUTPUT of
    // the plan (e09b-1's design note against 05 §8's inbound-`file` reading).
    out.file = read_string(data, "file");
    out.pointer = read_string(data, "pointer");
    out.raw_hash = parse_raw_hash(read_string(data, "rawHash"));
    ++writes_applied_;
    if (!read_bool(data, "reflected"))
    {
        // The daemon's own R-CLI-006 read-your-writes barrier (finish_edit -> query_after_hash) did
        // not confirm within its bound. The bytes ARE on disk — the write applied — but a caller
        // that immediately re-reads the DERIVED world may not see them yet. Counted rather than
        // swallowed; the Inspector's own re-read composes from disk, so it is unaffected.
        ++barrier_misses_;
        std::fprintf(stderr,
                     "context_editor: the daemon applied the edit to `%s` but its read barrier did "
                     "not confirm the derived world reflected it\n",
                     out.file.c_str());
    }
    return out;
}

inspector::FieldState WireOverrideWriteGateway::read(const std::string& root_scene,
                                                     const std::vector<std::string>& id_path,
                                                     const std::string& pointer) const
{
    inspector::FieldState out;
    if (client_ == nullptr)
    {
        // present:false + a 0 token. The L-30 engine compares this against the gesture's collision
        // base and — unless the base was itself null — DROPS. With NO client the engine never even
        // reaches here: `attempt` refused first, so the gesture ends as `Status::error` with the
        // staged edit kept (header § LIFETIME states which of the two closes the door, and the one
        // residual). A field we cannot read is a field we must not overwrite.
        return out;
    }

    contract::Json params = contract::Json::object();
    params.set("path", contract::Json(root_scene));
    params.set("idPath", contract::Json(join_id_path(id_path)));

    ++reads_issued_;
    std::string error;
    const std::optional<contract::Json> reply =
        client_->call("editor.inspect", std::move(params), error);
    if (!reply.has_value())
    {
        std::fprintf(stderr, "context_editor: the L-30 re-read (`editor.inspect`) was refused (%s: "
                             "%s); the gesture will be dropped rather than overwrite\n",
                     client_->last_error_code().c_str(), error.c_str());
        return out;
    }

    // The SAME hop + the SAME parser the Inspector feed hydrates through — literally the same
    // function (`parse_inspect_reply`), so the value this gateway compares against is byte-identical
    // to the value the panel is showing. A second hand-rolled reader here is exactly how a rebase
    // decision drifts from what the human saw.
    //
    // `editor.inspect`'s rawHash is the ROOT SCENE file's current raw-byte hash — the outermost
    // target's CAS token, which is what FieldState documents. A `template` / `at-instance` write
    // resolves a DIFFERENT file and no read verb reports a token for it, and the L-30 engine rebases
    // on THIS token unconditionally (inspector_panel.cpp discards the refusal's own `raw_hash`) — so
    // a retarget would guard the wrong file. Only `outermost` reaches here today, because that is the
    // one target `InspectorPanel::commit` sends; carrying the resolved target's token is a FieldState
    // seam change, tracked with the read-refusal residual in this file's header.
    const std::optional<inspector::InspectorModel> model = parse_inspect_reply(*reply, out.raw_hash);
    if (!model.has_value() || !model->has_entity)
    {
        return out; // present:false — the entity no longer resolves; fail closed.
    }
    for (const inspector::InspectorField& field : model->fields)
    {
        if (field.pointer == pointer)
        {
            out.present = true;
            out.value = field.value;
            break;
        }
    }
    return out;
}

} // namespace context::editor::shell::panels
