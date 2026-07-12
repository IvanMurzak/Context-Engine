// The net.* fail-closed error-code strings (M6 X2, R-NET-001 / L-48). SOURCE OF TRUTH for the codes
// the contract error catalog registers in its F0a-reserved net.* block — the same
// promote-a-local-string pattern as the physics3d/physics2d package blocks and runtime/js's
// gc_errors.h — so this runtime module never links the contract layer (the dependency direction
// stays runtime -> kernel, per the L-60 microkernel model). All deterministic refusals: a bare retry
// cannot repair an unassigned/duplicate network identity, a size-mismatched snapshot payload, or an
// authority conflict.

#pragma once

namespace context::runtime::netsync
{

// A replication registration used the unassigned network identity (net_id == 0); nothing was
// registered (validation class). net_id == 0 is the L-48 "unassigned" sentinel.
inline constexpr const char* kInvalidNetIdCode = "net.invalid_net_id";

// Two entities were registered for replication with the SAME network identity; the second was
// refused so the composed-id keyed mapping stays 1:1 (validation class).
inline constexpr const char* kDuplicateNetIdCode = "net.duplicate_net_id";

// A state-sync snapshot carried a component payload whose byte length disagrees with the replicated
// component set's declared size for that component id (a malformed / incompatible snapshot); nothing
// was applied (validation class).
inline constexpr const char* kSnapshotComponentMismatchCode = "net.snapshot_component_mismatch";

// A state-sync delta targeted an entity the replica itself holds authority over; the authoritative
// peer's local state wins, so the inbound delta is refused and the replica is left unchanged (usage
// class — an authority-arbitration conflict).
inline constexpr const char* kAuthorityConflictCode = "net.authority_conflict";

} // namespace context::runtime::netsync
