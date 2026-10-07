#include "sharedresultcache.h"
#include <cstdlib>
#include <future>
#include <string>

using namespace bn_graph_diff;
static void check(bool value) { if (!value) std::abort(); }

int main()
{
	SharedResultCache<int, std::string> cache(80, 2);
	auto a = std::make_shared<const std::string>("first corpus pair");
	auto b = std::make_shared<const std::string>("second corpus pair");
	auto c = std::make_shared<const std::string>("third corpus pair");
	cache.put(1, a, 40);
	cache.put(2, b, 40);
	cache.put(3, c, 40);
	check(cache.bytes() == 80 && cache.size() == 2 && cache.evictions() == 1);
	check(cache.current(1, a)); // Eviction cannot invalidate an open corpus row.
	check(cache.get(1) == a); // A lookup recovers the identical object, never a clone.
	check(cache.bytes() == 80 && cache.size() == 2);
	std::weak_ptr<const std::string> retired = b;
	b.reset();
	check(retired.expired()); // The non-owning registry does not retain payloads.
	check(!cache.get(2));
	check(cache.tracked() == 2);

	auto replacement = std::make_shared<const std::string>("reviewed pair");
	cache.put(1, replacement, 40);
	check(!cache.current(1, a) && cache.current(1, replacement));
	check(*a == "first corpus pair"); // Stale readers may finish without becoming current.
	cache.erase(1);
	check(!cache.current(1, replacement) && !cache.get(1));
	const auto startedAt = cache.revision();
	cache.erase(999);
	check(cache.revision() != startedAt); // Invalidation also rejects an in-flight miss.
	cache.eraseIf([](int key) { return key == 3; });
	check(!cache.current(3, c) && cache.bytes() == 0);

	// An oversized result survives worker-to-UI handoff without any cache owner.
	// The future owns the shared result while its completion is waiting for the UI.
	SharedResultCache<int, std::string> disabled(0, 2);
	auto future = std::async(std::launch::async, [&]() {
		auto result = std::make_shared<const std::string>("worker result");
		disabled.put(4, result, 100);
		return result;
	});
	future.wait(); // Synchronizes cache access; the cache deliberately has no internal lock.
	check(disabled.size() == 0 && disabled.bytes() == 0);
	{
		auto panel = future.get();
		check(disabled.get(4) == panel && disabled.current(4, panel));
	}
	check(!disabled.get(4));
	SharedResultCache<int, std::string> small(1, 1);
	small.put(1, a, 100);
	check(small.size() == 0 && small.current(1, a) && small.get(1) == a);

	// Closed-workspace metadata is pruned along with released payloads.
	for (int i = 0; i < 1000; ++i)
		disabled.put(i, std::make_shared<const std::string>("transient"), 100);
	check(disabled.bytes() == 0 && disabled.tracked() <= 1);
	return 0;
}
