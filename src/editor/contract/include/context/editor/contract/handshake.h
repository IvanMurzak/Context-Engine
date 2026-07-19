// Handshake: capability negotiation carrying {protocolMajor, capabilities[]} (R-CLI-004/010).
//
// From day one the attach exchange carries `{ protocolMajor, supportedCapabilities[],
// minClientProtocol }` and hard-fails (through the R-CLI-008 envelope) outside the compatibility
// window. At the M3 freeze the contract is FROZEN at protocolMajor == 1 (R-CLI-004): the surface
// no longer breaks without a deprecation cycle (R-CLI-010 governs every future change). The
// negotiation LOGIC (degrade to the intersected capability subset) was wired from day one so
// activating it at the 2nd protocol version is non-breaking; while only one major exists the window
// is exactly {kProtocolMajor}, so any other client major is an immediate hard-fail. The engine
// NEVER silently degrades below the negotiated subset.

#pragma once

#include "context/editor/contract/envelope.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace context::editor::contract
{

// The engine's current protocol major. FROZEN at 1 by the M3 contract freeze (R-CLI-004): the
// public surface is now stable and no longer breaks without a deprecation cycle — every future
// change is governed by the R-CLI-010 deprecation lifecycle (which ACTIVATES now that this != 0).
inline constexpr std::uint32_t kProtocolMajor = 1;

// The R-CLI-010 written deprecation policy: a deprecated verb / method / flag / capability survives
// at least this many MINOR protocol versions before it may be removed, giving scripts and agents a
// bounded migration window. The lifecycle is ACTIVE now that kProtocolMajor != 0 (the M3 freeze):
// the frozen surface may only change through a deprecation cycle. Surfaced in `context describe`
// under `contract.deprecationPolicy.minMinorsBeforeRemoval`.
inline constexpr std::uint32_t kDeprecationMinMinors = 2;

// The capabilities the daemon advertises in the handshake (R-CLI-010). Reserved, grep-stable names.
[[nodiscard]] const std::vector<std::string>& daemon_capabilities();

// What the client sends in the attach handshake.
struct ClientHandshake
{
    std::uint32_t protocol_major = kProtocolMajor;
    std::vector<std::string> capabilities; // capabilities the client understands
    // OPTIONAL attach token (D20, R-SEC-002). Additive under the frozen protocolMajor=1: a client
    // that omits it is byte-identical on the wire to a pre-D20 client. The daemon compares it against
    // `.editor/instance.json`'s token when attach-token enforcement is ON (default OFF for e01 — the
    // C-F1 sequencing: e02 migrates the CLI onto the client SDK, THEN enforcement defaults ON). When
    // enforcement is OFF the field is carried but never gated on (the M1 ambient-OS-guard trust model).
    std::string token;
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
