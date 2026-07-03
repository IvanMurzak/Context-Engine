// Handshake: capability negotiation carrying {protocolMajor, capabilities[]} (R-CLI-004/010).
//
// From day one the attach exchange carries `{ protocolMajor, supportedCapabilities[],
// minClientProtocol }` and hard-fails (through the R-CLI-008 envelope) outside the compatibility
// window — even though M1 ships UNSTABLE at protocolMajor == 0. The negotiation LOGIC (degrade to
// the intersected capability subset) is wired now so activating it at the 2nd protocol version is
// non-breaking; while only major 0 exists the window is exactly {0}, so any other client major is
// an immediate hard-fail. The engine NEVER silently degrades below the negotiated subset.

#pragma once

#include "context/editor/contract/envelope.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace context::editor::contract
{

// The engine's current protocol major. M1 is explicitly UNSTABLE: while this is 0 the contract MAY
// break without deprecation cycles (R-CLI-004). It bumps to 1 at the M3 freeze.
inline constexpr std::uint32_t kProtocolMajor = 0;

// The capabilities the daemon advertises in the handshake (R-CLI-010). Reserved, grep-stable names.
[[nodiscard]] const std::vector<std::string>& daemon_capabilities();

// What the client sends in the attach handshake.
struct ClientHandshake
{
    std::uint32_t protocol_major = kProtocolMajor;
    std::vector<std::string> capabilities; // capabilities the client understands
};

// A successful negotiation result: the agreed protocol major and the intersected capability subset.
struct Negotiated
{
    std::uint32_t protocol_major = kProtocolMajor;
    std::vector<std::string> capabilities; // client ∩ daemon, order-stable by daemon advertisement
};

// negotiate() returns the agreed subset on success, or a hard-fail Envelope (carrying the
// `handshake.incompatible_protocol` catalog code) when the client is outside the compatibility
// window. std::variant makes "you cannot use a negotiated result without checking for failure"
// structural rather than conventional.
using HandshakeResult = std::variant<Negotiated, Envelope>;

[[nodiscard]] HandshakeResult negotiate(const ClientHandshake& client);

// The daemon side of the handshake serialized for `context describe` / the wire (R-CLI-013): the
// `{ protocolMajor, stable, capabilities[], minClientProtocol }` object.
[[nodiscard]] Json protocol_descriptor();

} // namespace context::editor::contract
