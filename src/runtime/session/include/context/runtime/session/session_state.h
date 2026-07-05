// Canonical (de)serialization of a live Session's full runtime state (R-FILE-009 / R-QA-005).
//
// The one-shot CLI session-control verbs (`session new` -> `session step` -> `session hash`) are
// separate process invocations, so the session's full runtime state persists between them in a
// canonical JSON document (RuntimeKernel's OWN serialization — never an authored file, R-FILE-009;
// the same posture as a save). The document captures the seed, the PRNG state, the simTick counter,
// every entity + its integer components (by stable component name), and the recorded input stream,
// so re-loading and stepping is bit-identical to stepping straight through (the determinism law).
//
// Restore rebuilds the World by recreating entities in ascending-index order and asserting each
// recreated id matches the serialized (index, generation): the demo scenario never recycles ids, so
// this reproduces the exact World; a scenario that DID recycle would fail loudly (session.state_invalid)
// rather than silently restore a different-hashing world — the honest boundary until runtime
// spawn/despawn + an explicit id-restore primitive land.

#pragma once

#include "context/editor/serializer/json_tree.h"
#include "context/runtime/session/session.h"

#include <optional>
#include <string>
#include <string_view>

namespace context::runtime::session
{

namespace serializer = ::context::editor::serializer;

// Serialize the session's full runtime state to a canonical-JSON document tree.
[[nodiscard]] serializer::JsonValue session_state_to_json(const Session& session);

// Serialize to the canonical-JSON string (the bytes a `.session.json` state file holds).
[[nodiscard]] std::string session_state_dump(const Session& session);

struct LoadResult
{
    bool ok = false;
    std::optional<Session> session;
    std::string error_code; // e.g. "session.state_invalid" on failure
    std::string message;
};

// Rebuild a Session from a state document / string. On any structural problem returns ok=false with
// error_code="session.state_invalid".
[[nodiscard]] LoadResult session_from_json(const serializer::JsonValue& doc);
[[nodiscard]] LoadResult session_state_parse(std::string_view text);

} // namespace context::runtime::session
