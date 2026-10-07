#pragma once

#include "weightedcache.h"
#include <memory>

namespace bn_graph_diff {

// Caller-synchronized. Cache ownership is bounded; external readers keep the
// exact current object discoverable through a non-owning index after eviction.
template <typename Key, typename Value>
class SharedResultCache
{
  public:
	using Ref = std::shared_ptr<const Value>;

  private:
	struct Live { std::weak_ptr<const Value> value; size_t bytes; };
	std::map<Key, Live> m_live;
	WeightedLruCache<Key, Ref> m_owned;
	uint64_t m_revision = 0;

	void pruneExpired()
	{
		for (auto it = m_live.begin(); it != m_live.end();)
			if (it->second.value.expired()) it = m_live.erase(it);
			else ++it;
	}

  public:
	SharedResultCache(size_t budget, size_t maximumEntries) : m_owned(budget, maximumEntries) {}
	Ref get(const Key& key)
	{
		if (auto owned = m_owned.get(key)) return *owned;
		auto live = m_live.find(key);
		if (live == m_live.end()) return {};
		auto value = live->second.value.lock();
		if (!value) { m_live.erase(live); return {}; }
		m_owned.put(key, value, live->second.bytes);
		return value;
	}
	bool current(const Key& key, const Ref& expected) const
	{
		auto live = m_live.find(key);
		return expected && live != m_live.end() && live->second.value.lock() == expected;
	}
	void put(const Key& key, const Ref& value, size_t bytes)
	{
		m_live.insert_or_assign(key, Live {value, bytes});
		m_owned.put(key, value, bytes);
		pruneExpired();
	}
	void erase(const Key& key)
	{
		++m_revision; // Also rejects work started before invalidating a missing key.
		m_live.erase(key);
		m_owned.erase(key);
	}
	template <typename Predicate> void eraseIf(Predicate predicate)
	{
		++m_revision;
		for (auto it = m_live.begin(); it != m_live.end();)
			if (predicate(it->first))
			{
				m_owned.erase(it->first);
				it = m_live.erase(it);
			}
			else ++it;
	}
	uint64_t revision() const { return m_revision; }
	size_t bytes() const { return m_owned.bytes(); }
	size_t size() const { return m_owned.size(); }
	size_t tracked() const { return m_live.size(); }
	size_t evictions() const { return m_owned.evictions(); }
};

} // namespace bn_graph_diff
