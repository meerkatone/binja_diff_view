#include "weightedcache.h"
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace bn_graph_diff;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "Failed: %s at %d\n", #condition, __LINE__); std::abort(); } } while (0)

int main()
{
    WeightedLruCache<int, std::shared_ptr<int>> cache(100, 3);
    auto a = std::make_shared<int>(1), b = std::make_shared<int>(2), c = std::make_shared<int>(3);
    std::weak_ptr<int> weakB = b;
    CHECK(cache.put(1, a, 40));
    CHECK(cache.put(2, b, 40));
    b.reset();
    CHECK(cache.get(1).value() == a); // Refresh A, so B is the eviction victim.
    CHECK(cache.put(3, c, 40));
    CHECK(!cache.get(2));
    CHECK(weakB.expired());
    CHECK(cache.bytes() == 80 && cache.evictions() == 1);

    auto active = cache.get(1).value();
    cache.setBudget(20);
    CHECK(cache.size() == 0 && cache.bytes() == 0);
    CHECK(*active == 1); // Eviction must not invalidate an active comparison.
    CHECK(!cache.put(1, a, 21)); // Oversized snapshots remain caller-owned only.

    cache.setBudget(100);
    CHECK(cache.put(1, a, 20));
    CHECK(cache.put(1, c, 30));
    CHECK(!cache.updateWeight(1, a, 90)); // A stale preparation cannot replace C.
    CHECK(cache.bytes() == 30 && cache.get(1).value() == c);
    CHECK(cache.updateWeight(1, c, 95));
    CHECK(cache.bytes() == 95);
    CHECK(!cache.updateWeight(1, c, 101));
    CHECK(cache.size() == 0 && cache.bytes() == 0);

    WeightedLruCache<int, int> countBounded(1000, 1);
    CHECK(countBounded.put(1, 10, 0));
    CHECK(countBounded.put(2, 20, 0));
    CHECK(!countBounded.get(1) && countBounded.size() == 1);
    countBounded.setBudget(0);
    CHECK(!countBounded.put(3, 30, 0) && countBounded.size() == 0);
    std::puts("Weighted cache tests passed");
}
