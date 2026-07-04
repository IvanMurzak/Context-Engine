// Stable intra-file ids (L-33): collision-resistant random, >= 64-bit, file-scoped — never
// sequential. Child collections are id-keyed arrays-of-objects-with-`id` (the R-FILE-001 form).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace context::editor::compose
{

// The well-known scene-root entity token (L-35). A scene's root entity is addressable in override
// id-paths as its explicit stable id when one is authored, or as this reserved token otherwise.
// Deliberately NOT a valid stable id ('$' is not lowercase hex), so it can never collide with a
// minted id and is never produced by mint_stable_id().
inline constexpr std::string_view kSceneRootId = "$root";

// True iff `id` has the stable-id FORM: 16..32 lowercase hex characters (64..128 bits). The form
// check is what the composition layer enforces (L-33 pins >= 64-bit and file-scoped uniqueness;
// uniqueness is checked per file by the scene model, not here). Sequential-looking values that
// happen to be valid hex still pass — "never sequential" is an allocation rule (mint_stable_id
// draws random bits), not a recognizable property of a single value.
[[nodiscard]] bool is_stable_id(std::string_view id) noexcept;

// Format 64 random bits as the canonical 16-char lowercase-hex stable id (zero-padded).
[[nodiscard]] std::string format_stable_id(std::uint64_t bits);

// Mint a fresh 64-bit stable id from a non-deterministic random source (std::random_device,
// stirred with a monotonic clock tick so a degenerate random_device cannot yield a fixed
// sequence). File-scoped uniqueness is the caller's check (duplicate ids in one file are a
// compose.duplicate_id diagnostic); at 64 random bits, cross-worktree collisions on genuinely
// distinct entities are the L-33 "collide only on genuinely-shared entities" property.
[[nodiscard]] std::string mint_stable_id();

} // namespace context::editor::compose
