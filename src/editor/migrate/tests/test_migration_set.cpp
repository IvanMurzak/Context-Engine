// MigrationSet registry tests: registration validation (happy + every refusal class), lookup,
// and the set hash's sensitivity + registration-order-independence (the R-FILE-005 pass-0 hash
// that keys pass-1 derivation cache entries).

#include "migrate_test.h"

#include "context/editor/migrate/migration_set.h"

#include <optional>
#include <string>
#include <utility>

using namespace context::editor::migrate;
using migratetest::register_reference_steps;

namespace
{

MigrationStep step(std::string type, std::int64_t from, MigrationTier tier = MigrationTier::engine_native,
                   std::uint64_t revision = 1, std::string wasm = std::string())
{
    MigrationStep s;
    s.component_type = std::move(type);
    s.from_version = from;
    s.tier = tier;
    s.revision = revision;
    s.transform = [](context::editor::serializer::JsonValue&) { return true; };
    s.wasm_module = std::move(wasm);
    return s;
}

void test_registration_validation()
{
    MigrationSet set;
    std::string problem;

    // Happy path.
    CHECK(set.register_component("test:sprite", 3, problem));
    CHECK(set.register_step(step("test:sprite", 1), problem));
    CHECK(set.register_step(step("test:sprite", 2), problem));
    CHECK(problem.empty());

    // Idempotent re-add of a component replaces the current version.
    CHECK(set.register_component("test:sprite", 4, problem));
    CHECK(set.current_version("test:sprite") == 4);

    // Refusals: malformed type ids.
    CHECK(!set.register_component("sprite", 1, problem));       // no namespace
    CHECK(!set.register_component(":sprite", 1, problem));      // empty namespace
    CHECK(!set.register_component("test:", 1, problem));        // empty name
    CHECK(!set.register_component("test:thing", 0, problem));   // version < 1
    CHECK(!set.register_step(step("plain", 1), problem));       // step: no namespace
    CHECK(!set.register_step(step("test:sprite", 0), problem)); // from < 1

    // Refusal: revision < 1.
    MigrationStep bad_rev = step("test:other", 1);
    bad_rev.revision = 0;
    CHECK(!set.register_step(std::move(bad_rev), problem));

    // Refusal: engine-native step with no transform.
    MigrationStep no_fn = step("test:other", 1);
    no_fn.transform = nullptr;
    CHECK(!set.register_step(std::move(no_fn), problem));

    // Refusal: package-sandboxed step with no wasm module reference.
    MigrationStep no_wasm = step("pkg:thing", 1, MigrationTier::package_sandboxed);
    CHECK(!set.register_step(std::move(no_wasm), problem));

    // A package-sandboxed step WITH a module reference registers (execution is tier-gated later).
    CHECK(set.register_step(step("pkg:thing", 1, MigrationTier::package_sandboxed, 1, "pkg/mig.wasm"),
                            problem));

    // Refusal: duplicate (type, from) — steps are write-once.
    CHECK(!set.register_step(step("test:sprite", 1), problem));
    CHECK(!problem.empty());
}

void test_lookup()
{
    MigrationSet set;
    register_reference_steps(set);

    CHECK(set.current_version("test:sprite") == 3);
    CHECK(set.current_version("test:unknown") == 0);
    CHECK(set.current_version("ctx:transform") == 0);

    CHECK(set.find_step("test:sprite", 1) != nullptr);
    CHECK(set.find_step("test:sprite", 2) != nullptr);
    CHECK(set.find_step("test:sprite", 3) == nullptr); // current: no step FROM the top
    CHECK(set.find_step("test:unknown", 1) == nullptr);
    CHECK(!set.empty());
    CHECK(MigrationSet().empty());
}

void test_set_hash_properties()
{
    std::string problem;

    // Deterministic + stable for the empty set (the engine set today).
    CHECK(MigrationSet().set_hash() == MigrationSet().set_hash());
    CHECK(MigrationSet::engine_set().set_hash() == MigrationSet().set_hash());

    // Sensitivity: each identity-bearing field changes the hash.
    MigrationSet base;
    CHECK(base.register_component("test:sprite", 2, problem));
    CHECK(base.register_step(step("test:sprite", 1), problem));

    MigrationSet other_current;
    CHECK(other_current.register_component("test:sprite", 3, problem));
    CHECK(other_current.register_step(step("test:sprite", 1), problem));
    CHECK(base.set_hash() != other_current.set_hash());

    MigrationSet other_revision;
    CHECK(other_revision.register_component("test:sprite", 2, problem));
    CHECK(other_revision.register_step(step("test:sprite", 1, MigrationTier::engine_native, 2),
                                       problem));
    CHECK(base.set_hash() != other_revision.set_hash());

    MigrationSet extra_step;
    CHECK(extra_step.register_component("test:sprite", 2, problem));
    CHECK(extra_step.register_step(step("test:sprite", 1), problem));
    CHECK(extra_step.register_step(step("test:other", 1), problem));
    CHECK(base.set_hash() != extra_step.set_hash());

    MigrationSet other_tier;
    CHECK(other_tier.register_component("test:sprite", 2, problem));
    CHECK(other_tier.register_step(
        step("test:sprite", 1, MigrationTier::package_sandboxed, 1, "m.wasm"), problem));
    CHECK(base.set_hash() != other_tier.set_hash());

    // A different wasm module alone changes the hash (same tier/revision).
    MigrationSet wasm_a;
    CHECK(wasm_a.register_component("pkg:t", 2, problem));
    CHECK(wasm_a.register_step(step("pkg:t", 1, MigrationTier::package_sandboxed, 1, "a.wasm"),
                               problem));
    MigrationSet wasm_b;
    CHECK(wasm_b.register_component("pkg:t", 2, problem));
    CHECK(wasm_b.register_step(step("pkg:t", 1, MigrationTier::package_sandboxed, 1, "b.wasm"),
                               problem));
    CHECK(wasm_a.set_hash() != wasm_b.set_hash());

    // Registration-order independence: the same set registered in a different order hashes equal.
    MigrationSet order_a;
    CHECK(order_a.register_component("test:sprite", 3, problem));
    CHECK(order_a.register_component("test:other", 2, problem));
    CHECK(order_a.register_step(step("test:sprite", 1), problem));
    CHECK(order_a.register_step(step("test:sprite", 2), problem));
    CHECK(order_a.register_step(step("test:other", 1), problem));

    MigrationSet order_b;
    CHECK(order_b.register_step(step("test:other", 1), problem));
    CHECK(order_b.register_component("test:other", 2, problem));
    CHECK(order_b.register_step(step("test:sprite", 2), problem));
    CHECK(order_b.register_component("test:sprite", 3, problem));
    CHECK(order_b.register_step(step("test:sprite", 1), problem));

    CHECK(order_a.set_hash() == order_b.set_hash());
    CHECK(problem.empty());
}

void test_hash_combine_basics()
{
    // The fold terminator keeps ("ab","c") and ("a","bc") apart.
    const std::uint64_t ab_c = hash_combine(hash_combine(0, std::string_view("ab")),
                                            std::string_view("c"));
    const std::uint64_t a_bc = hash_combine(hash_combine(0, std::string_view("a")),
                                            std::string_view("bc"));
    CHECK(ab_c != a_bc);
    CHECK(hash_combine(0, std::uint64_t{1}) != hash_combine(0, std::uint64_t{2}));
}

} // namespace

int main()
{
    test_registration_validation();
    test_lookup();
    test_set_hash_properties();
    test_hash_combine_basics();
    MIGRATE_TEST_MAIN_END();
}
