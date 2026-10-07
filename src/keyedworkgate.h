#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace bn_graph_diff {

// Serialize cache misses for one key without holding a global/cache lock during
// work. Callers recheck their cache and capture revision epochs AFTER admission.
// A failed/cancelled owner releases the gate so another caller can retry.
template <typename Key>
class KeyedWorkGate
{
	std::mutex m_mutex;
	std::map<Key, std::weak_ptr<std::timed_mutex>> m_active;

  public:
	class Lease
	{
		// Declaration order keeps the mutex alive until the lock is released.
		std::shared_ptr<std::timed_mutex> m_mutex;
		std::unique_lock<std::timed_mutex> m_lock;
		friend class KeyedWorkGate;
		Lease(std::shared_ptr<std::timed_mutex> mutex,
		    std::unique_lock<std::timed_mutex> lock)
		    : m_mutex(std::move(mutex)), m_lock(std::move(lock)) {}

	  public:
		Lease() = default;
		Lease(Lease&&) = default;
		Lease& operator=(Lease&&) = delete;
		explicit operator bool() const { return m_lock.owns_lock(); }
	};

	template <typename Cancelled>
	Lease acquire(const Key& key, Cancelled cancelled)
	{
		if (cancelled()) return {};
		std::shared_ptr<std::timed_mutex> mutex;
		{
			std::lock_guard<std::mutex> indexLock(m_mutex);
			// Retain only active owners/waiters, plus expired entries until the
			// next admission. This registry does not grow with completed work.
			for (auto it = m_active.begin(); it != m_active.end();)
				if (it->second.expired()) it = m_active.erase(it);
				else ++it;
			auto& slot = m_active[key];
			mutex = slot.lock();
			if (!mutex)
			{
				mutex = std::make_shared<std::timed_mutex>();
				slot = mutex;
			}
		}
		std::unique_lock<std::timed_mutex> workLock(*mutex, std::defer_lock);
		while (!workLock.try_lock_for(std::chrono::milliseconds(50)))
			if (cancelled()) return {};
		if (cancelled()) return {};
		return Lease(std::move(mutex), std::move(workLock));
	}

	size_t tracked()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_active.size();
	}
};

} // namespace bn_graph_diff
