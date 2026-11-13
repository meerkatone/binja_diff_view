#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace bn_graph_diff {

// The functions that analysis reported as changed while a view was being
// extracted, by view. Between begin() and the end of absorption the extraction
// owns those reports and re-extracts the functions itself; outside it absorb()
// declines them and the caller handles the change.
template <typename Key>
class ExtractionSettling
{
  public:
	void begin(const Key& key)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_changed[key];
	}
	void end(const Key& key)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_changed.erase(key);
	}
	bool absorb(const Key& key, uint64_t start)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto found = m_changed.find(key);
		if (found == m_changed.end())
			return false;
		found->second.insert(start);
		return true;
	}
	std::set<uint64_t> take(const Key& key)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto found = m_changed.find(key);
		return found == m_changed.end() ? std::set<uint64_t> {} : std::exchange(found->second, {});
	}
	// A change is either returned here or, once this returns nothing and
	// absorption has ended under the same lock, declined by absorb().
	std::set<uint64_t> takeOrEnd(const Key& key)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto found = m_changed.find(key);
		if (found == m_changed.end())
			return {};
		std::set<uint64_t> changed = std::move(found->second);
		if (changed.empty())
			m_changed.erase(found);
		else
			found->second.clear();
		return changed;
	}

  private:
	std::mutex m_mutex;
	std::map<Key, std::set<uint64_t>> m_changed;
};

}  // namespace bn_graph_diff
