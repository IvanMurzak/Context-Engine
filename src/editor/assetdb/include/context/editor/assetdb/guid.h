// Asset GUIDs (L-36): immutable 128-bit identity, minted once per asset in <asset>.meta.json.

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace context::editor::assetdb
{

// The pinned GUID text form: exactly 32 lowercase hex characters (128 bits). Collision-resistant
// random (L-33's >= 64-bit rule, doubled for the project-lifetime asset namespace); never
// sequential, so parallel worktrees minting GUIDs collide only with negligible probability.
[[nodiscard]] bool is_guid(std::string_view s) noexcept;

// Render 128 bits as the pinned 32-hex-char form.
[[nodiscard]] std::string format_guid(std::uint64_t hi, std::uint64_t lo);

// The GUID-minting seam. Injectable (R-QA-010 spirit: no hidden nondeterminism in the layer) so the
// deterministic tests drive a fixed sequence while production mints randomly.
class GuidGenerator
{
public:
    virtual ~GuidGenerator() = default;
    // A fresh GUID in the pinned form. Uniqueness against EXISTING assets is the caller's job
    // (the AssetDatabase checks its index; the astronomically-unlikely random collision surfaces
    // as the duplicate-GUID diagnostic rather than silent identity aliasing).
    [[nodiscard]] virtual std::string next() = 0;
};

// Production generator: 128 random bits per GUID from a random_device-seeded engine.
class RandomGuidGenerator final : public GuidGenerator
{
public:
    RandomGuidGenerator();
    [[nodiscard]] std::string next() override;

private:
    std::mt19937_64 rng_;
};

// Deterministic test generator: guid(counter), well-formed and strictly increasing.
class SequenceGuidGenerator final : public GuidGenerator
{
public:
    explicit SequenceGuidGenerator(std::uint64_t start = 1) : next_(start) {}
    [[nodiscard]] std::string next() override;

private:
    std::uint64_t next_;
};

} // namespace context::editor::assetdb
