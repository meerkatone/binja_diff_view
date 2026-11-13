#include "extractionsettling.h"
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace bn_graph_diff;
static void check(bool value) { if (!value) std::abort(); }

int main()
{
	ExtractionSettling<int> settling;
	// Outside an extraction the change belongs to the caller.
	check(!settling.absorb(1, 0x1000));
	check(settling.take(1).empty() && settling.takeOrEnd(1).empty());

	settling.begin(1);
	check(settling.absorb(1, 0x1000) && settling.absorb(1, 0x2000) && settling.absorb(1, 0x1000));
	check(!settling.absorb(2, 0x1000)); // Another view is not being extracted.
	check((settling.take(1) == std::set<uint64_t> {0x1000, 0x2000}));
	check(settling.take(1).empty());
	check(settling.absorb(1, 0x3000)); // take() leaves absorption open.

	// A round that reports changes keeps absorbing; an empty round ends it.
	check((settling.takeOrEnd(1) == std::set<uint64_t> {0x3000}));
	check(settling.absorb(1, 0x4000));
	check((settling.takeOrEnd(1) == std::set<uint64_t> {0x4000}));
	check(settling.takeOrEnd(1).empty());
	check(!settling.absorb(1, 0x5000));

	settling.begin(1);
	check(settling.absorb(1, 0x6000));
	settling.end(1); // An abandoned extraction drops what it absorbed.
	check(!settling.absorb(1, 0x6000));
	check(settling.takeOrEnd(1).empty());

	// Every report is either returned by a round or declined after the last one.
	settling.begin(7);
	std::atomic<bool> stop {false};
	std::atomic<uint64_t> absorbed {0};
	std::vector<std::thread> reporters;
	for (int thread = 0; thread < 4; thread++)
		reporters.emplace_back([&, thread]() {
			for (uint64_t next = 0; !stop; next++)
				if (settling.absorb(7, (uint64_t(thread) << 32) | next))
					absorbed++;
		});
	uint64_t taken = 0;
	while (absorbed < 1000)
		taken += settling.take(7).size();
	for (;;)
	{
		auto changed = settling.takeOrEnd(7);
		if (changed.empty())
			break;
		taken += changed.size();
	}
	// Absorption ended under the lock that found the round empty, so reporters
	// still running are declined and nothing they absorbed is left behind.
	check(!settling.absorb(7, 1));
	stop = true;
	for (auto& reporter : reporters)
		reporter.join();
	check(taken == absorbed);
	return 0;
}
