// Client-schema projection (see schema.h).

#include "context/editor/client/schema.h"

#include "context/editor/contract/handshake.h"
#include "context/editor/contract/registry.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace context::editor::client
{

using contract::Json;

contract::Json client_schema()
{
    // `describe()` returns the whole self-description wrapped in a single `contract` member (the
    // shape `context describe --json` prints under data.contract). Unwrap it so the projected
    // sections sit at the client schema's top level, where a generated binding expects them.
    const Json document = contract::Registry::instance().describe();
    const Json& described =
        document.contains("contract") ? document.at("contract") : document;

    Json out = Json::object();
    out.set("schemaVersion", Json(static_cast<std::int64_t>(kClientSchemaVersion)));
    out.set("generatedFrom", Json(std::string("contract::Registry::describe")));
    out.set("protocolMajor", Json(static_cast<std::uint64_t>(contract::kProtocolMajor)));

    // The client-relevant sections, in a fixed order so the emitted bytes are stable.
    static constexpr const char* kProjectedSections[] = {
        "protocol",     "rpcMethods",    "eventTopics",   "eventEnvelope",
        "subscription", "errorCatalog",  "largeResult",   "queryLanguage",
        "fileKinds",    "componentTypes", "deprecationPolicy"};
    for (const char* section : kProjectedSections)
    {
        const std::string key(section);
        if (described.contains(key))
            out.set(key, described.at(key));
    }

    // Declare the omission IN the artifact. `generatedFrom` alone reads as "this IS describe's
    // output", but this is a proper SUBSET: a binding generator that assumed completeness would
    // silently emit nothing for the rest, and a reader diffing against `context describe` would see
    // phantom drift.
    //
    // DERIVED, not hardcoded (describe's sections MINUS the projected ones). A literal list would
    // name only the sections known when it was typed, so a future 12th section would land in
    // NEITHER list — silently dropped from the projection AND absent here, making the artifact
    // assert a completeness that is false, which is the exact failure this field exists to prevent.
    // Deriving it also turns the drift gate into a tripwire that NOTICES a new section.
    // Today this yields the CLI-grammar surface (`verbs`, `coreFlags`) and the MCP adapter's tool
    // list — none of which a wire client consumes, which is why excluding them keeps an unrelated
    // CLI change from reading as client-contract drift.
    Json excluded = Json::array();
    for (const std::pair<std::string, Json>& member : described.object_members())
    {
        const bool projected =
            std::any_of(std::begin(kProjectedSections), std::end(kProjectedSections),
                        [&member](const char* section) { return member.first == section; });
        if (!projected)
            excluded.push_back(Json(member.first));
    }
    out.set("excludedSections", std::move(excluded));
    return out;
}

std::string client_schema_text()
{
    return client_schema().dump(2) + "\n";
}

} // namespace context::editor::client
