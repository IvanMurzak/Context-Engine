// Sim-component registry + read/write/hash round-trips (R-QA-013).

#include "context/runtime/session/hash.h"
#include "context/runtime/session/sim_component.h"
#include "session_test.h"

using namespace context::runtime::session;
namespace kernel = context::kernel;

int main()
{
    const SimComponentRegistry& reg = builtin_components();

    // --- the built-in set is registered with stable names + ordered fields ---------------------
    {
        const SimComponentType* pos = reg.by_name("position");
        CHECK(pos != nullptr);
        CHECK(pos->fields.size() == 2);
        CHECK(pos->fields[0] == "x");
        CHECK(pos->fields[1] == "y");

        const SimComponentType* input = reg.by_name("input_state");
        CHECK(input != nullptr);
        CHECK(input->fields.size() == 5);

        CHECK(reg.by_name("velocity") != nullptr);
        CHECK(reg.by_name("health") != nullptr);
        CHECK(reg.by_name("nonexistent") == nullptr);

        // by_id agrees with by_name (id was assigned at registration).
        CHECK(reg.by_id(pos->id) == pos);
    }

    // --- read / write / hash walk the component's int64 fields --------------------------------
    {
        const SimComponentType* pos = reg.by_name("position");
        Position p{3, 7};
        const std::vector<SimField> fields = pos->read(&p);
        CHECK(fields.size() == 2);
        CHECK(fields[0].name == "x");
        CHECK(fields[0].value == 3);
        CHECK(fields[1].value == 7);

        // write overwrites the stored fields in declared order.
        pos->write(&p, {11, 22});
        CHECK(p.x == 11);
        CHECK(p.y == 22);

        // hash folds every field: two equal components hash equal; a changed field changes it.
        Fnv1a h1;
        pos->hash(&p, h1);
        Position q{11, 22};
        Fnv1a h2;
        pos->hash(&q, h2);
        CHECK(h1.digest() == h2.digest());
        Position r{11, 23};
        Fnv1a h3;
        pos->hash(&r, h3);
        CHECK(h1.digest() != h3.digest());
    }

    // --- add_fn materializes a default component on an entity ----------------------------------
    {
        const SimComponentType* health = reg.by_name("health");
        kernel::World w;
        const kernel::Entity e = w.create();
        void* storage = health->add_fn(w, e);
        CHECK(storage != nullptr);
        health->write(storage, {250});
        CHECK(w.get<Health>(e)->hp == 250);
        CHECK(health->locate_fn(w, e) == w.get<Health>(e));
    }

    // --- fixed-endian integer folding is deterministic ----------------------------------------
    {
        Fnv1a a;
        a.update_u64(0x0102030405060708ULL);
        Fnv1a b;
        b.update_u64(0x0102030405060708ULL);
        CHECK(a.digest() == b.digest());
        Fnv1a c;
        c.update_u64(0x0102030405060709ULL);
        CHECK(a.digest() != c.digest());
    }

    SESSION_TEST_MAIN_END();
}
