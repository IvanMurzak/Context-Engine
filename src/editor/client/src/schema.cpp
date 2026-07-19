// Client-schema projection (see schema.h).

#include "context/editor/client/schema.h"

#include "context/editor/contract/handshake.h"
#include "context/editor/contract/registry.h"

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
    for (const char* section : {"protocol", "rpcMethods", "eventTopics", "eventEnvelope",
                                "subscription", "errorCatalog", "largeResult", "queryLanguage",
                                "fileKinds", "componentTypes", "deprecationPolicy"})
    {
        const std::string key(section);
        if (described.contains(key))
            out.set(key, described.at(key));
    }
    return out;
}

std::string client_schema_text()
{
    return client_schema().dump(2) + "\n";
}

} // namespace context::editor::client
