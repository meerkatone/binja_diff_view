#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace bn_graph_diff {

// Caller supplies synchronization and allocation estimates. Eviction releases
// only the cache's ownership; a shared payload remains valid for active readers.
template <typename Key, typename Value>
class WeightedLruCache
{
    struct Entry { Value value; size_t bytes; uint64_t accessed; };
    std::map<Key, Entry> m_entries;
    size_t m_budget, m_maxEntries, m_bytes = 0, m_evictions = 0;
    uint64_t m_clock = 0;

    void evictOldest()
    {
        auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
            [](const auto& a, const auto& b) { return a.second.accessed < b.second.accessed; });
        m_bytes -= oldest->second.bytes;
        m_entries.erase(oldest);
        ++m_evictions;
    }

  public:
    WeightedLruCache(size_t budget, size_t maxEntries) : m_budget(budget), m_maxEntries(maxEntries) {}

    std::optional<Value> get(const Key& key)
    {
        auto found = m_entries.find(key);
        if (found == m_entries.end()) return {};
        found->second.accessed = ++m_clock;
        return found->second.value;
    }

    void erase(const Key& key)
    {
        auto found = m_entries.find(key);
        if (found == m_entries.end()) return;
        m_bytes -= found->second.bytes;
        m_entries.erase(found);
    }

    bool put(const Key& key, Value value, size_t bytes)
    {
        erase(key);
        if (bytes > m_budget || m_maxEntries == 0 || m_budget == 0) return false;
        while (m_bytes > m_budget - bytes || m_entries.size() >= m_maxEntries)
            evictOldest();
        m_entries.emplace(key, Entry {std::move(value), bytes, ++m_clock});
        m_bytes += bytes;
        return true;
    }

    // A completed preparation must never reweight or replace a newer revision.
    bool updateWeight(const Key& key, const Value& expected, size_t bytes)
    {
        auto found = m_entries.find(key);
        if (found == m_entries.end() || found->second.value != expected) return false;
        return put(key, expected, bytes);
    }

    void setBudget(size_t budget)
    {
        m_budget = budget;
        while (!m_entries.empty() && (m_bytes > m_budget || m_budget == 0)) evictOldest();
    }

    size_t size() const { return m_entries.size(); }
    size_t bytes() const { return m_bytes; }
    size_t budget() const { return m_budget; }
    size_t evictions() const { return m_evictions; }
};

} // namespace bn_graph_diff
