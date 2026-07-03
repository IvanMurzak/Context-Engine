// Minimal M1 placeholder for the canonical parse step — see canonical_parse.h.

#include "context/editor/derivation/canonical_parse.h"

namespace context::editor::derivation
{

namespace
{

// ASCII whitespace only — intentionally locale-independent so canonicalization is deterministic
// across platforms (a hard requirement for content-hash memoization, L-22).
[[nodiscard]] bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

} // namespace

std::uint64_t canonical_hash_of(std::string_view bytes) noexcept
{
    // FNV-1a 64-bit. Placeholder for the M2 canonical-content hash.
    std::uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    for (unsigned char byte : bytes)
    {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

CanonicalForm canonical_parse(std::string_view source_bytes)
{
    std::string normalized;
    normalized.reserve(source_bytes.size());

    bool pending_space = false; // a run of whitespace collapses to at most one emitted space
    bool seen_non_space = false;
    for (char c : source_bytes)
    {
        if (is_space(c))
        {
            pending_space = seen_non_space; // never emit leading whitespace
            continue;
        }
        if (pending_space)
        {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(c);
        seen_non_space = true;
    }
    // A trailing run of whitespace leaves `pending_space` set but is intentionally dropped (trim).

    CanonicalForm form;
    form.canonical_hash = canonical_hash_of(normalized);
    form.bytes = std::move(normalized);
    return form;
}

} // namespace context::editor::derivation
