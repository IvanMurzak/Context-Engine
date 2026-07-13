// Migration-set registry implementation (see migration_set.h).

#include "context/editor/migrate/migration_set.h"

#include <algorithm>

namespace context::editor::migrate
{

namespace
{
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

bool valid_type_id(std::string_view type)
{
    const std::size_t colon = type.find(':');
    return colon != std::string_view::npos && colon > 0 && colon + 1 < type.size();
}
} // namespace

std::uint64_t hash_combine(std::uint64_t seed, std::string_view bytes) noexcept
{
    std::uint64_t h = seed == 0 ? kFnvOffset : seed;
    for (const char c : bytes)
    {
        h ^= static_cast<unsigned char>(c);
        h *= kFnvPrime;
    }
    // A terminator step so ("ab","c") and ("a","bc") fold differently.
    h ^= 0xffU;
    h *= kFnvPrime;
    return h;
}

std::uint64_t hash_combine(std::uint64_t seed, std::uint64_t value) noexcept
{
    std::uint64_t h = seed == 0 ? kFnvOffset : seed;
    for (int i = 0; i < 8; ++i)
    {
        h ^= (value >> (i * 8)) & 0xffU;
        h *= kFnvPrime;
    }
    return h;
}

bool MigrationSet::register_component(std::string component_type, std::int64_t current_version,
                                      std::string& problem)
{
    if (!valid_type_id(component_type))
    {
        problem += "component type \"" + component_type + "\" is not a namespaced \"<ns>:<type>\" id; ";
        return false;
    }
    if (current_version < 1)
    {
        problem += "component \"" + component_type + "\" current version must be >= 1; ";
        return false;
    }
    for (ComponentVersion& existing : components_)
        if (existing.type == component_type)
        {
            existing.current = current_version; // idempotent re-add (the SchemaSet pattern)
            return true;
        }
    components_.push_back({std::move(component_type), current_version});
    return true;
}

bool MigrationSet::register_step(MigrationStep step, std::string& problem)
{
    if (!valid_type_id(step.component_type))
    {
        problem += "step component type \"" + step.component_type +
                   "\" is not a namespaced \"<ns>:<type>\" id; ";
        return false;
    }
    if (step.from_version < 1)
    {
        problem += "step \"" + step.component_type + "\" from_version must be >= 1; ";
        return false;
    }
    if (step.revision < 1)
    {
        problem += "step \"" + step.component_type + "\" revision must be >= 1; ";
        return false;
    }
    if (step.tier == MigrationTier::engine_native && !step.transform)
    {
        problem += "engine-native step \"" + step.component_type + "\" v" +
                   std::to_string(step.from_version) + " carries no transform; ";
        return false;
    }
    if (step.tier == MigrationTier::package_sandboxed && step.wasm_module.empty())
    {
        problem += "package-sandboxed step \"" + step.component_type + "\" v" +
                   std::to_string(step.from_version) + " carries no wasm module reference; ";
        return false;
    }
    if (find_step(step.component_type, step.from_version) != nullptr)
    {
        problem += "duplicate step for \"" + step.component_type + "\" from v" +
                   std::to_string(step.from_version) +
                   " (steps are write-once; a behavior change bumps `revision`); ";
        return false;
    }
    steps_.push_back(std::move(step));
    return true;
}

std::int64_t MigrationSet::current_version(std::string_view component_type) const noexcept
{
    for (const ComponentVersion& c : components_)
        if (c.type == component_type)
            return c.current;
    return 0;
}

const MigrationStep* MigrationSet::find_step(std::string_view component_type,
                                             std::int64_t from_version) const noexcept
{
    for (const MigrationStep& s : steps_)
        if (s.from_version == from_version && s.component_type == component_type)
            return &s;
    return nullptr;
}

std::uint64_t MigrationSet::set_hash() const noexcept
{
    // Registration-order-insensitive: XOR-fold per-entry chained hashes. Every identity-bearing
    // field of every entry participates, so ANY set change — a new step, a bumped revision, a
    // changed current version, a different wasm module — yields a different hash, which yields
    // different pass-1 derivation cache keys (R-FILE-005/010).
    std::uint64_t folded = 0x9e3779b97f4a7c15ULL; // non-zero seed: the EMPTY set has a stable hash
    for (const ComponentVersion& c : components_)
    {
        std::uint64_t h = hash_combine(0, std::string_view("component"));
        h = hash_combine(h, c.type);
        h = hash_combine(h, static_cast<std::uint64_t>(c.current));
        folded ^= h;
    }
    for (const MigrationStep& s : steps_)
    {
        std::uint64_t h = hash_combine(0, std::string_view("step"));
        h = hash_combine(h, s.component_type);
        h = hash_combine(h, static_cast<std::uint64_t>(s.from_version));
        h = hash_combine(h, static_cast<std::uint64_t>(s.tier));
        h = hash_combine(h, s.revision);
        h = hash_combine(h, s.wasm_module);
        // Fold the module CONTENT hash, not only the reference string (issue #71 PR4 / R-FILE-010):
        // two package versions shipping DIFFERENT bytes under the SAME wasm_module reference would
        // otherwise collide to the same set hash — and serve pass-1 derived state computed under the
        // old module. The content hash is 0 for engine_native steps (and for a package step whose
        // bytes are not yet resolved); GUARD on non-zero so this is a genuine no-op there and never
        // perturbs existing hashes (hash_combine(h, 0) is NOT the identity — it still folds 8 zero
        // bytes through the FNV prime, so an unconditional fold would rekey every engine_native set).
        if (s.wasm_module_hash != 0)
            h = hash_combine(h, s.wasm_module_hash);
        folded ^= h;
    }
    return folded;
}

const MigrationSet& MigrationSet::engine_set()
{
    // EMPTY today: no engine component-payload schema has ever bumped. The first real bump
    // registers its step here WITH its R-QA-011 fixture pair (see fixtures/README.md) in the same
    // PR — the fixture is a deliverable of the schema bump itself, kept forever.
    static const MigrationSet the_set;
    return the_set;
}

} // namespace context::editor::migrate
