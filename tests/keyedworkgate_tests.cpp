#include "keyedworkgate.h"
#include <atomic>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace bn_graph_diff;
static void check(bool value) { if (!value) std::abort(); }
static bool neverCancelled() { return false; }

int main()
{
	KeyedWorkGate<int> gate;
	{
		auto owner = gate.acquire(1, neverCancelled);
		check(bool(owner));
		auto other = gate.acquire(2, neverCancelled);
		check(bool(other)); // Unrelated work must progress while key 1 is held.
		auto waiter = std::async(std::launch::async, [&]() {
			int polls = 0;
			auto cancelled = gate.acquire(1, [&]() { return ++polls >= 3; });
			check(!cancelled && polls == 3);
		});
		check(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
		waiter.get(); // Cancellation completes while the original owner still holds its lease.
		check(bool(owner));
	}
	check(!gate.acquire(1, []() { return true; }));
	{
		auto retry = gate.acquire(1, neverCancelled);
		check(bool(retry));
	}

	// Eight overlapping cache requests perform work once and reuse the same object.
	// The value is accessed only with the key held, as in the engine's cache recheck.
	std::shared_ptr<const int> cached;
	std::atomic<int> ready {0};
	int executions = 0;
	std::promise<void> start;
	auto startSignal = start.get_future().share();
	std::vector<std::future<std::shared_ptr<const int>>> requests;
	for (int i = 0; i < 8; ++i)
		requests.push_back(std::async(std::launch::async, [&]() {
			++ready;
			startSignal.wait();
			auto lease = gate.acquire(3, neverCancelled);
			check(bool(lease));
			if (!cached)
			{
				++executions;
				cached = std::make_shared<const int>(42);
			}
			return cached;
		}));
	while (ready != 8) std::this_thread::yield();
	start.set_value();
	for (auto& request : requests)
	{
		auto result = request.get();
		check(result == cached);
	}
	check(executions == 1);

	// Throwing work must not strand waiters or poison the next attempt.
	try
	{
		auto lease = gate.acquire(4, neverCancelled);
		throw std::runtime_error("test owner failure");
	}
	catch (const std::runtime_error&) {}
	{
		auto retry = gate.acquire(4, neverCancelled);
		check(bool(retry));
		auto moved = std::move(retry);
		check(bool(moved) && !retry);
	}
	for (int i = 0; i < 1000; ++i)
	{
		auto lease = gate.acquire(i, neverCancelled);
		check(bool(lease) && gate.tracked() == 1);
	}
	return 0;
}
