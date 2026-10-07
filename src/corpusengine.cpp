#include "corpusengine.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>

#include <algorithm>
#include <exception>
#include <map>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <unistd.h>
#endif

#include "diffmodel.h"

using namespace BinaryNinja;

namespace bn_graph_diff {

namespace {

uint64_t currentResidentBytesImpl()
{
#if defined(__APPLE__)
	mach_task_basic_info_data_t information {};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
	        reinterpret_cast<task_info_t>(&information), &count) != KERN_SUCCESS)
		return 0;
	return information.resident_size;
#elif defined(__linux__)
	std::ifstream status("/proc/self/statm");
	uint64_t totalPages = 0;
	uint64_t residentPages = 0;
	if (!(status >> totalPages >> residentPages))
		return 0;
	(void)totalPages;
	long pageSize = sysconf(_SC_PAGESIZE);
	return pageSize > 0 ? residentPages * static_cast<uint64_t>(pageSize) : 0;
#else
	return 0;
#endif
}

const CorpusBinaryInput* findBinary(
	const std::vector<CorpusBinaryInput>& binaries, CorpusBinaryId id)
{
	auto found = std::find_if(binaries.begin(), binaries.end(),
	    [id](const CorpusBinaryInput& binary) { return binary.metadata.id == id; });
	return found == binaries.end() ? nullptr : &*found;
}

CorpusFunctionFlags functionFlags(BinaryViewRef view)
{
	CorpusFunctionFlags result;
	if (!view)
		return result;
	for (const auto& function : view->GetAnalysisFunctionList())
	{
		if (!function)
			continue;
		CorpusFunctionFilterFlags flags;
		Ref<Symbol> symbol = function->GetSymbol();
		if (symbol)
		{
			const std::string shortName = symbol->GetShortName();
			flags.thunk = shortName.rfind("j_", 0) == 0;
			BNSymbolType symbolType = symbol->GetType();
			flags.externalStub = symbolType == ImportAddressSymbol
			    || symbolType == ImportedFunctionSymbol || symbolType == ExternalSymbol
			    || symbolType == SymbolicFunctionSymbol;
		}
		result[function->GetStart()] = flags;
	}
	return result;
}


}  // namespace

uint64_t currentProcessResidentBytes()
{
	return currentResidentBytesImpl();
}

CorpusFunctionFlags captureCorpusFunctionFlags(BinaryViewRef view)
{
	return functionFlags(view);
}

CorpusPairMetrics calculateCorpusPairMetrics(const FunctionMatchMaps& maps,
	BinaryViewRef source, BinaryViewRef target, const CorpusMetricFilters& filters)
{
	CorpusMetricFlagBatch flags(filters);
	const auto& sourceFlags = flags.get(0, [&]() { return captureCorpusFunctionFlags(source); });
	const auto& targetFlags = flags.get(1, [&]() { return captureCorpusFunctionFlags(target); });
	return calculateCorpusPairMetricsWithFlags(maps, sourceFlags, targetFlags, filters);
}

namespace {

template <typename Callback>
void publishToGui(const QPointer<CorpusScheduler>& scheduler, Callback callback)
{
	if (!scheduler)
		return;
	QMetaObject::invokeMethod(scheduler, [scheduler, callback = std::move(callback)]() {
		if (scheduler)
			callback(scheduler);
	}, Qt::QueuedConnection);
}

CorpusResultRef runCorpus(const CorpusRunRequest& request,
	Ref<BackgroundTask> task, const QPointer<CorpusScheduler>& scheduler)
{
	auto result = std::make_shared<CorpusResult>();
	for (const auto& binary : request.binaries)
		result->binaries.push_back(binary.metadata);

	const int total = static_cast<int>(request.plan.edges.size());
	int completed = 0;
	for (size_t edgeIndex = 0; edgeIndex < request.plan.edges.size(); edgeIndex++)
	{
		const CorpusEdgeKey edge = request.plan.edges[edgeIndex];
		QElapsedTimer pairTimer;
		pairTimer.start();
		if (task && task->IsCancelled())
		{
			result->partial = true;
			for (; edgeIndex < request.plan.edges.size(); edgeIndex++)
			{
				auto cancelled = std::make_shared<CorpusPairResult>();
				cancelled->edge = request.plan.edges[edgeIndex];
				cancelled->state = CorpusPairState::Cancelled;
				cancelled->error = "Corpus analysis cancelled before this pair started";
				result->pairs[cancelled->edge] = cancelled;
				publishToGui(scheduler, [cancelled, completed, total](CorpusScheduler* target) {
					emit target->pairResultReady(cancelled, completed, total);
				});
			}
			break;
		}

		auto pair = std::make_shared<CorpusPairResult>();
		pair->edge = edge;
		const CorpusBinaryInput* source = findBinary(request.binaries, edge.source);
		const CorpusBinaryInput* target = findBinary(request.binaries, edge.target);
		if (!source || !target || !source->view || !target->view)
		{
			pair->state = CorpusPairState::Failed;
			pair->error = "A selected BinaryView is no longer available";
		}
		else
		{
			FunctionMatchMapsRef cached;
			if (request.reuseCachedPairs)
				cached = DiffEngine::instance().cachedFunctionMatchesRef(
				    source->view, target->view);
			else
				DiffEngine::instance().invalidateMatchPair(source->view, target->view);
			publishToGui(scheduler, [edge, completed, total, cached](CorpusScheduler* current) {
				emit current->pairStarted(edge.source, edge.target,
				    completed, total, cached != nullptr);
			});

			if (cached)
			{
				pair->state = CorpusPairState::Cached;
				pair->matches = cached;
			}
			else
			{
				if (task)
					task->SetProgressText("Binja Diff: corpus pair "
					    + std::to_string(edgeIndex + 1) + "/" + std::to_string(total));
				try
				{
					FunctionMatchMapsRef maps = DiffEngine::instance().matchFunctionsRef(
					    source->view, target->view, task);
					if (maps->valid)
					{
						pair->state = CorpusPairState::Complete;
						pair->matches = std::move(maps);
					}
					else
					{
						pair->state = task && task->IsCancelled()
						    ? CorpusPairState::Cancelled : CorpusPairState::Failed;
						pair->error = maps->error;
					}
				}
				catch (const std::exception& exception)
				{
					pair->state = CorpusPairState::Failed;
					pair->error = std::string("Function matching failed: ") + exception.what();
				}
				catch (...)
				{
					pair->state = CorpusPairState::Failed;
					pair->error = "Function matching failed with an unknown worker exception";
				}
			}
			if (pair->matches)
			{
				pair->rawMetrics = calculateCorpusPairMetrics(
				    *pair->matches, source->view, target->view);
				pair->metrics = pair->rawMetrics;
				updateCorpusBinaryCounts(result->binaries, edge, pair->rawMetrics);
			}
		}

		if (pair->state == CorpusPairState::Cancelled)
			result->partial = true;
		pair->elapsedMicroseconds = static_cast<uint64_t>(pairTimer.nsecsElapsed() / 1000);
		pair->elapsedMilliseconds = pair->elapsedMicroseconds / 1000;
		result->pairs[edge] = pair;
		completed++;
		publishToGui(scheduler, [pair, completed, total](CorpusScheduler* current) {
			emit current->pairResultReady(pair, completed, total);
		});
	}
	return result;
}

}  // namespace

CorpusScheduler::CorpusScheduler(QObject* parent) : QObject(parent)
{
	qRegisterMetaType<CorpusPairResultRef>();
	qRegisterMetaType<CorpusResultRef>();
	m_memoryTimer = new QTimer(this);
	m_memoryTimer->setInterval(50);
	connect(m_memoryTimer, &QTimer::timeout, this, [this]() {
		m_peakResidentBytes = std::max(m_peakResidentBytes, currentProcessResidentBytes());
	});
}

CorpusScheduler::~CorpusScheduler()
{
	cancel();
	if (m_watcher && m_watcher->isRunning())
		m_watcher->waitForFinished();
}

bool CorpusScheduler::start(const CorpusRunRequest& request)
{
	if (m_running || !request.plan.valid)
		return false;
	m_running = true;
	m_startingResidentBytes = currentProcessResidentBytes();
	m_peakResidentBytes = m_startingResidentBytes;
	m_runTimer.start();
	m_memoryTimer->start();
	m_task = new BackgroundTask("Binja Diff: preparing corpus analysis", true);
	m_watcher = new QFutureWatcher<CorpusResultRef>(this);
	connect(m_watcher, &QFutureWatcher<CorpusResultRef>::finished, this, [this]() {
		CorpusResultRef result;
		try
		{
			result = m_watcher->result();
		}
		catch (...)
		{
			auto failed = std::make_shared<CorpusResult>();
			failed->partial = true;
			result = failed;
		}
		if (m_task)
			m_task->Finish();
		m_task = nullptr;
		m_memoryTimer->stop();
		m_peakResidentBytes = std::max(m_peakResidentBytes, currentProcessResidentBytes());
		auto measured = std::make_shared<CorpusResult>(result ? *result : CorpusResult {});
		measured->elapsedMicroseconds = static_cast<uint64_t>(m_runTimer.nsecsElapsed() / 1000);
		measured->startingResidentBytes = m_startingResidentBytes;
		measured->peakResidentBytes = m_peakResidentBytes;
		result = measured;
		m_running = false;
		emit runFinished(result);
		m_watcher->deleteLater();
		m_watcher = nullptr;
	});

	QPointer<CorpusScheduler> scheduler(this);
	Ref<BackgroundTask> task = m_task;
	m_watcher->setFuture(QtConcurrent::run(
	    [request, task, scheduler]() { return runCorpus(request, task, scheduler); }));
	return true;
}

void CorpusScheduler::cancel()
{
	if (m_task && !m_task->IsFinished())
		m_task->Cancel();
}

}  // namespace bn_graph_diff
