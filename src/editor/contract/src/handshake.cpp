// Handshake / capability-negotiation implementation (see handshake.h).

#include "context/editor/contract/handshake.h"

namespace context::editor::contract
{

const std::vector<std::string>& daemon_capabilities()
{
    // The advertised capability set. Reserved, grep-stable names; grows additively with the
    // surface. These mirror contract features clients can branch on (R-CLI-010).
    static const std::vector<std::string> caps = {
        "describe",      // R-CLI-013 whole-contract self-description
        "result-envelope", // R-CLI-008 uniform envelope + catalog
        "verb-grammar",  // R-CLI-007 [<ns>:]<noun> <verb> grammar + core flags
        "dry-run",       // the --dry-run core flag is honored
        "if-match",      // R-CLI-006 CAS on --if-match
    };
    return caps;
}

HandshakeResult negotiate(const ClientHandshake& client)
{
    // Compatibility window: while only one major exists, the window is exactly {kProtocolMajor}. A
    // client on any other major hard-fails through the R-CLI-008 envelope (never a silent degrade).
    if (client.protocol_major != kProtocolMajor)
    {
        return Envelope::failure(
            "handshake.incompatible_protocol",
            "Client protocol major " + std::to_string(client.protocol_major) +
                " is outside the daemon's compatibility window {" + std::to_string(kProtocolMajor) +
                "}. Use the Project-pinned engine or upgrade the client.");
    }

    // In-window: degrade to the intersection of client and daemon capabilities, ordered by the
    // daemon's advertisement so the negotiated subset is deterministic.
    Negotiated agreed;
    agreed.protocol_major = kProtocolMajor;
    for (const std::string& cap : daemon_capabilities())
    {
        for (const std::string& want : client.capabilities)
        {
            if (cap == want)
            {
                agreed.capabilities.push_back(cap);
                break;
            }
        }
    }
    return agreed;
}

Json protocol_descriptor()
{
    Json out = Json::object();
    out.set("protocolMajor", Json(static_cast<std::uint64_t>(kProtocolMajor)));
    out.set("stable", Json(kProtocolMajor != 0));
    out.set("minClientProtocol", Json(static_cast<std::uint64_t>(kProtocolMajor)));
    Json caps = Json::array();
    for (const std::string& c : daemon_capabilities())
        caps.push_back(Json(c));
    out.set("capabilities", std::move(caps));
    out.set("note", Json("FROZEN at protocolMajor=1 (M3 contract freeze, R-CLI-004) — the surface "
                         "is stable and changes only through the R-CLI-010 deprecation lifecycle."));
    return out;
}

} // namespace context::editor::contract
