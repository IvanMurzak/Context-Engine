// Resource-handle tests: generation-checked handles, staleness, slot reuse (R-QA-013).

#include "context/kernel/resource.h"
#include "kernel_test.h"

#include <string>

using namespace context::kernel;

namespace
{
struct Mesh
{
    int triangles = 0;
};
} // namespace

int main()
{
    // --- insert / get / size -------------------------------------------------------------------
    {
        ResourcePool<Mesh> pool;
        CHECK(pool.empty());
        const Handle<Mesh> h = pool.insert(Mesh{12});
        CHECK(h.valid());
        CHECK(pool.size() == 1);
        CHECK(pool.contains(h));
        Mesh* m = pool.get(h);
        CHECK(m != nullptr);
        CHECK(m->triangles == 12);

        // Mutating through the pointer sticks.
        m->triangles = 30;
        CHECK(pool.get(h)->triangles == 30);
    }

    // --- invalid / null handle -----------------------------------------------------------------
    {
        ResourcePool<Mesh> pool;
        CHECK(!Handle<Mesh>{}.valid());
        CHECK(pool.get(Handle<Mesh>{}) == nullptr);
        CHECK(!pool.contains(Handle<Mesh>{}));
    }

    // --- erase makes the handle stale ----------------------------------------------------------
    {
        ResourcePool<Mesh> pool;
        const Handle<Mesh> h = pool.insert(Mesh{1});
        CHECK(pool.erase(h));
        CHECK(pool.size() == 0);
        CHECK(pool.get(h) == nullptr); // stale
        CHECK(!pool.contains(h));
        CHECK(!pool.erase(h)); // double-erase fails cleanly
    }

    // --- slot reuse: the old handle stays stale, the new one is valid --------------------------
    {
        ResourcePool<Mesh> pool;
        const Handle<Mesh> a = pool.insert(Mesh{1});
        CHECK(pool.erase(a));
        const Handle<Mesh> b = pool.insert(Mesh{2});
        CHECK(b.index == a.index);           // slot recycled
        CHECK(b.generation != a.generation); // but a different generation
        CHECK(pool.get(b) != nullptr);
        CHECK(pool.get(b)->triangles == 2);
        CHECK(pool.get(a) == nullptr);       // the old handle never resurrects
    }

    // --- many resources + const access ---------------------------------------------------------
    {
        ResourcePool<std::string> pool;
        const Handle<std::string> h1 = pool.insert("alpha");
        const Handle<std::string> h2 = pool.insert("beta");
        CHECK(pool.size() == 2);
        const ResourcePool<std::string>& cpool = pool;
        CHECK(*cpool.get(h1) == "alpha");
        CHECK(*cpool.get(h2) == "beta");
    }

    KERNEL_TEST_MAIN_END();
}
