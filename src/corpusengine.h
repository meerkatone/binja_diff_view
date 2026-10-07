// Serial corpus scheduler over the existing pair matcher.
#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <memory>

#include "corpusmodel.h"
#include "diffengine.h"

namespace bn_graph_diff {

struct CorpusBinaryInput
{
	CorpusBinary metadata;
	BinaryViewRef view;
};

struct CorpusRunRequest
{
	std::vector<CorpusBinaryInput> binaries;
	CorpusPlan plan;
	bool reuseCachedPairs = true;
};

struct CorpusMetricFilters
{
	size_t minimumInstructionCount = 0;
	bool excludeThunks = false;
	bool excludeExternalStubs = false;
	bool excludeSmallGeneratedFunctions = false;
	size_t generatedFunctionMaximumInstructions = 4;
	bool includeExactMatches = true;
};

struct CorpusFunctionFilterFlags
{
	bool thunk = false;
	bool externalStub = false;
};
using CorpusFunctionFlags = std::map<uint64_t, CorpusFunctionFilterFlags>;

// Scoped to one synchronous filter application. Never retains symbol flags
// across event turns or analysis revisions.
class CorpusMetricFlagBatch
{
	bool m_required;
	std::map<CorpusBinaryId, CorpusFunctionFlags> m_flags;

  public:
	explicit CorpusMetricFlagBatch(const CorpusMetricFilters& filters)
	    : m_required(filters.excludeThunks || filters.excludeExternalStubs) {}
	template <typename Capture>
	const CorpusFunctionFlags& get(CorpusBinaryId binary, Capture capture)
	{
		static const CorpusFunctionFlags empty;
		if (!m_required) return empty;
		auto found = m_flags.find(binary);
		if (found == m_flags.end()) found = m_flags.emplace(binary, capture()).first;
		return found->second;
	}
};

CorpusFunctionFlags captureCorpusFunctionFlags(BinaryViewRef view);

struct CorpusPairResult
{
	CorpusEdgeKey edge;
	CorpusPairState state = CorpusPairState::Queued;
	FunctionMatchMapsRef matches;
	CorpusPairMetrics rawMetrics;
	CorpusPairMetrics metrics;
	CorpusMetricFilters metricFilters;
	uint64_t elapsedMilliseconds = 0;
	uint64_t elapsedMicroseconds = 0;
	std::string error;
};

using CorpusPairResultRef = std::shared_ptr<const CorpusPairResult>;

struct CorpusResult
{
	std::vector<CorpusBinary> binaries;
	std::map<CorpusEdgeKey, CorpusPairResultRef> pairs;
	bool partial = false;
	uint64_t elapsedMicroseconds = 0;
	uint64_t startingResidentBytes = 0;
	uint64_t peakResidentBytes = 0;
};

using CorpusResultRef = std::shared_ptr<const CorpusResult>;

CorpusPairMetrics calculateCorpusPairMetrics(const FunctionMatchMaps& maps,
	BinaryViewRef source, BinaryViewRef target,
	const CorpusMetricFilters& filters = {});
CorpusPairMetrics calculateCorpusPairMetricsWithFlags(const FunctionMatchMaps& maps,
	const CorpusFunctionFlags& source, const CorpusFunctionFlags& target,
	const CorpusMetricFilters& filters = {});

uint64_t currentProcessResidentBytes();

class CorpusScheduler : public QObject
{
	Q_OBJECT

  public:
	explicit CorpusScheduler(QObject* parent = nullptr);
	~CorpusScheduler() override;

	bool start(const CorpusRunRequest& request);
	void cancel();
	bool running() const { return m_running; }

  signals:
	void pairStarted(uint32_t source, uint32_t target, int completed,
		int total, bool cached);
	void pairResultReady(bn_graph_diff::CorpusPairResultRef result,
		int completed, int total);
	void runFinished(bn_graph_diff::CorpusResultRef result);

  private:
	QFutureWatcher<CorpusResultRef>* m_watcher = nullptr;
	BinaryNinja::Ref<BinaryNinja::BackgroundTask> m_task;
	QElapsedTimer m_runTimer;
	QTimer* m_memoryTimer = nullptr;
	uint64_t m_startingResidentBytes = 0;
	uint64_t m_peakResidentBytes = 0;
	bool m_running = false;
};

}  // namespace bn_graph_diff

Q_DECLARE_METATYPE(bn_graph_diff::CorpusPairResultRef)
Q_DECLARE_METATYPE(bn_graph_diff::CorpusResultRef)
