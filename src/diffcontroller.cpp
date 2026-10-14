#include "diffcontroller.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <limits>

#include <QtWidgets/QApplication>
#include <QtCore/QFutureWatcher>
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include "diffview.h"
#include "symbolport.h"
#include "syncgroup.h"

using namespace BinaryNinja;
using namespace bn_graph_diff;

namespace {

using FunctionByAddress = std::map<uint64_t, FunctionRef>;

FunctionByAddress functionsByAddress(BinaryViewRef view)
{
	FunctionByAddress result;
	if (!view)
		return result;
	for (const auto& function : view->GetAnalysisFunctionList())
		if (function)
			result.emplace(function->GetStart(), function);
	return result;
}

void fillCandidateNames(SymbolPortCandidate& candidate, bool sideA, FunctionRef function)
{
	if (sideA)
		candidate.functionAExists = function != nullptr;
	else
		candidate.functionBExists = function != nullptr;
	if (!function)
		return;
	Ref<Symbol> symbol = function->GetSymbol();
	if (!symbol)
		return;
	if (sideA)
	{
		candidate.shortNameA = symbol->GetShortName();
		candidate.fullNameA = symbol->GetFullName();
		candidate.rawNameA = symbol->GetRawName();
	}
	else
	{
		candidate.shortNameB = symbol->GetShortName();
		candidate.fullNameB = symbol->GetFullName();
		candidate.rawNameB = symbol->GetRawName();
	}
}

FunctionRef displayedFunction(DiffViewContainer* pane)
{
	if (!pane || !pane->data() || !pane->graphView())
		return nullptr;

	// DisassemblyView::getCurrentFunction can briefly lag the graph displayed
	// after programmatic navigation. Resolve the cursor address first so an old
	// function (commonly main) cannot receive a symbol intended for a newly
	// displayed thunk. Prefer an exact function start, then the innermost
	// containing function, and only then fall back to the view's current value.
	uint64_t offset = pane->graphView()->getCurrentOffset();
	auto exact = pane->data()->GetAnalysisFunctionsForAddress(offset);
	if (!exact.empty())
	{
		FunctionRef current = pane->graphView()->getCurrentFunction();
		if (current)
			for (const auto& function : exact)
				if (function && function->GetStart() == current->GetStart())
					return function;
		return exact.front();
	}

	auto containing = pane->data()->GetAnalysisFunctionsContainingAddress(offset);
	FunctionRef best;
	for (const auto& function : containing)
		if (function && (!best || function->GetStart() > best->GetStart()))
			best = function;
	return best ? best : pane->graphView()->getCurrentFunction();
}

bool paneIsCurrent(DiffViewContainer* pane)
{
	return pane && pane->frame() && pane->graphView()
	    && pane->frame()->getCurrentViewInterface() == pane->getView();
}

bool analysisIdle(DiffViewContainer* pane)
{
	return pane && pane->data() && pane->data()->GetAnalysisState() == IdleState;
}

void refreshMatchNames(FunctionMatchMaps& maps, BinaryViewRef a, BinaryViewRef b)
{
	FunctionByAddress functionsA = functionsByAddress(a);
	FunctionByAddress functionsB = functionsByAddress(b);
	auto update = [](FunctionSummary& summary, const FunctionByAddress& functions) {
		auto it = functions.find(summary.address);
		if (it == functions.end() || !it->second)
			return;
		Ref<Symbol> symbol = it->second->GetSymbol();
		summary.name = functionMatchName(it->second->GetView(), symbol);
	};
	for (auto& match : maps.matches)
	{
		update(match.a, functionsA);
		update(match.b, functionsB);
	}
	for (auto& function : maps.unmatchedA)
		update(function, functionsA);
	for (auto& function : maps.unmatchedB)
		update(function, functionsB);
	rebuildCalleeRenameMap(maps);
}

void addManualMatchesForPlan(FunctionMatchMaps& maps, const SymbolPortPlan& plan,
	SymbolPortDirection direction)
{
	auto findSummary = [](std::vector<FunctionSummary>& functions, uint64_t address) {
		return std::find_if(functions.begin(), functions.end(),
		    [address](const FunctionSummary& function) { return function.address == address; });
	};
	for (const auto& rename : plan.renames)
	{
		uint64_t addressA = direction == SymbolPortDirection::AToB
		    ? rename.sourceAddress : rename.targetAddress;
		uint64_t addressB = direction == SymbolPortDirection::AToB
		    ? rename.targetAddress : rename.sourceAddress;
		if (maps.funcMap.count(addressA))
			continue;
		auto unmatchedA = findSummary(maps.unmatchedA, addressA);
		auto unmatchedB = findSummary(maps.unmatchedB, addressB);
		if (unmatchedA == maps.unmatchedA.end() || unmatchedB == maps.unmatchedB.end())
			continue;
		FunctionMatchRecord record;
		record.a = *unmatchedA;
		record.b = *unmatchedB;
		record.confidence = 1.0;
		record.matchType = "Manual";
		maps.matches.push_back(std::move(record));
		maps.funcMap[addressA] = addressB;
		if (addressA != addressB)
		{
			char from[32], to[32];
			snprintf(from, sizeof(from), "0x%llx", (unsigned long long)addressA);
			snprintf(to, sizeof(to), "0x%llx", (unsigned long long)addressB);
			maps.addrMap[from] = to;
		}
		maps.unmatchedA.erase(unmatchedA);
		maps.unmatchedB.erase(unmatchedB);
	}
	maps.matchCount = maps.matches.size();
	rebuildCalleeRenameMap(maps);
}

}  // namespace

// Overlay palette. Custom RGB rather than the BN standard highlight colors,
// which sit too close together (red vs orange) once tinted onto graph nodes.
static BNHighlightColor customColor(uint8_t r, uint8_t g, uint8_t b)
{
	BNHighlightColor color {};
	color.style = CustomHighlightColor;
	color.r = r;
	color.g = g;
	color.b = b;
	color.alpha = 255;
	return color;
}
const DiffPalette& diffPalette()
{
	static const DiffPalette standard = {
	    customColor(190, 45, 50),    // strong red
	    customColor(40, 150, 70),    // strong green
	    customColor(215, 180, 30),   // amber
	    customColor(85, 110, 200)};  // slate blue
	// Okabe-Ito derived: vermillion / blue / yellow / grey.
	static const DiffPalette colorblind = {
	    customColor(213, 94, 0),
	    customColor(0, 114, 178),
	    customColor(240, 228, 66),
	    customColor(150, 150, 150)};
	bool safe = false;
	try
	{
		safe = Settings::Instance()->Get<bool>("binjaDiffView.colorblindPalette");
	}
	catch (...)
	{}
	return safe ? colorblind : standard;
}

QString ilDisplayName(const FunctionViewType& type)
{
	switch (type.type)
	{
	case NormalFunctionGraph:
		return "Disassembly";
	case LowLevelILFunctionGraph:
		return "LLIL";
	case LiftedILFunctionGraph:
		return "Lifted IL";
	case LowLevelILSSAFormFunctionGraph:
		return "LLIL SSA";
	case MediumLevelILFunctionGraph:
		return "MLIL";
	case MediumLevelILSSAFormFunctionGraph:
		return "MLIL SSA";
	case MappedMediumLevelILFunctionGraph:
		return "Mapped MLIL";
	case MappedMediumLevelILSSAFormFunctionGraph:
		return "Mapped MLIL SSA";
	case HighLevelILFunctionGraph:
		return "HLIL";
	case HighLevelILSSAFormFunctionGraph:
		return "HLIL SSA";
	case HighLevelLanguageRepresentationFunctionGraph:
		return type.name.empty() ? QString("Pseudo C") : QString::fromStdString(type.name);
	default:
		return "Disassembly";
	}
}

DiffController& DiffController::instance()
{
	static DiffController* controller = new DiffController();
	return *controller;
}

DiffController::DiffController(QObject* parent) :
    QObject(parent),
    BinaryDataNotification(BinaryDataUpdates | FunctionUpdates | SymbolUpdates)
{
	m_syncTimer = new QTimer(this);
	m_syncTimer->setInterval(250);
	connect(m_syncTimer, &QTimer::timeout, this, &DiffController::syncTimerTick);

	// instance() may first be touched from a background (analysis) thread, which
	// would leave this object and its QTimer with that thread's affinity. A QTimer
	// can only be started from the thread it lives on, so pin the controller (and
	// its child timer) to the GUI thread; otherwise m_syncTimer->start() no-ops.
	// Done last so the timer child moves along with the controller.
	if (QApplication::instance() && thread() != QApplication::instance()->thread())
		moveToThread(QApplication::instance()->thread());
}

void DiffController::registerPane(DiffViewContainer* pane)
{
	m_panes.insert(pane);
	if (pane && pane->data())
	{
		invalidateOverlayCache(pane->data());
		BNBinaryView* object = pane->data()->GetObject();
		auto it = m_observedViews.find(object);
		if (it == m_observedViews.end())
		{
			pane->data()->RegisterNotification(this);
			m_observedViews.emplace(object, std::make_pair(pane->data(), 1));
		}
		else
			it->second.second++;
	}
}

void DiffController::unregisterPane(DiffViewContainer* pane)
{
	m_panes.erase(pane);
	if (pane == m_lastDriver)
		m_lastDriver = nullptr;
	if (pane->frame())
	{
		m_explicitPartners.erase(pane->frame());
		for (auto it = m_explicitPartners.begin(); it != m_explicitPartners.end();)
		{
			if (it->second.frame == pane->frame())
				it = m_explicitPartners.erase(it);
			else
				++it;
		}
	}
	if (pane == m_paneA || pane == m_paneB)
		clear();
	if (pane && pane->data())
	{
		BNBinaryView* object = pane->data()->GetObject();
		auto it = m_observedViews.find(object);
		if (it != m_observedViews.end() && --it->second.second == 0)
		{
			it->second.first->UnregisterNotification(this);
			DiffEngine::instance().invalidateMatches(it->second.first);
			m_observedViews.erase(it);
		}
	}
}

void DiffController::invalidateView(BinaryView* view)
{
	invalidateOverlayCache(view);
	if (!view)
		return;
	DiffEngine::instance().invalidateMatches(BinaryViewRef(view));
}

void DiffController::invalidateSymbolView(BinaryView* view)
{
	invalidateOverlayCache(view);
	if (!view)
		return;
	if (m_symbolPortTarget.load() == view->GetObject())
		DiffEngine::instance().invalidateExtraction(BinaryViewRef(view));
	else
		DiffEngine::instance().invalidateMatches(BinaryViewRef(view));
}

void DiffController::invalidateOverlayCache(BinaryView* view)
{
	if (!view)
		return;
	uintptr_t identity = reinterpret_cast<uintptr_t>(view->GetObject());
	std::lock_guard<std::mutex> lock(m_overlayCacheMutex);
	++m_overlayRevision;
	m_overlayCache.remove_if([identity](const OverlayCacheEntry& entry) {
		return entry.key.viewA == identity || entry.key.viewB == identity;
	});
}

std::optional<DiffController::OverlayCacheEntry> DiffController::cachedOverlay(
    const OverlayCacheKey& key)
{
	std::lock_guard<std::mutex> lock(m_overlayCacheMutex);
	auto found = std::find_if(m_overlayCache.begin(), m_overlayCache.end(),
	    [&key](const OverlayCacheEntry& entry) { return entry.key == key; });
	if (found == m_overlayCache.end())
		return std::nullopt;
	m_overlayCache.splice(m_overlayCache.begin(), m_overlayCache, found);
	return m_overlayCache.front();
}

bool DiffController::cacheOverlay(OverlayCacheEntry entry, uint64_t revision)
{
	std::lock_guard<std::mutex> lock(m_overlayCacheMutex);
	if (revision != m_overlayRevision.load())
		return false;
	m_overlayCache.remove_if(
	    [&entry](const OverlayCacheEntry& current) { return current.key == entry.key; });
	m_overlayCache.push_front(std::move(entry));
	while (m_overlayCache.size() > OverlayCacheLimit)
		m_overlayCache.pop_back();
	return true;
}

void DiffController::OnAnalysisFunctionAdded(BinaryView* view, Function*) { invalidateView(view); }
void DiffController::OnAnalysisFunctionRemoved(BinaryView* view, Function*) { invalidateView(view); }
// Whole-binary extraction requests each function's IL, which restores lazily
// loaded analysis and reports a function update on the requesting thread.
// Nothing an extraction or match depends on has changed; invalidating there
// would discard the extraction that caused the update and restart it
// indefinitely, whether or not the database has unsaved edits.
void DiffController::OnAnalysisFunctionUpdated(BinaryView* view, Function*)
{
	if (!materializingFunctionAnalysis())
		invalidateView(view);
}
void DiffController::OnSymbolAdded(BinaryView* view, Symbol*) { invalidateSymbolView(view); }
void DiffController::OnSymbolRemoved(BinaryView* view, Symbol*) { invalidateSymbolView(view); }
void DiffController::OnSymbolUpdated(BinaryView* view, Symbol*) { invalidateSymbolView(view); }
void DiffController::OnBinaryDataWritten(BinaryView* view, uint64_t, size_t) { invalidateView(view); }
void DiffController::OnBinaryDataInserted(BinaryView* view, uint64_t, size_t) { invalidateView(view); }
void DiffController::OnBinaryDataRemoved(BinaryView* view, uint64_t, uint64_t) { invalidateView(view); }

void DiffController::pairFrames(ViewFrame* left, ViewFrame* right)
{
	if (!left || !right || left == right)
		return;
	m_explicitPartners[left] = {right, true};
	m_explicitPartners[right] = {left, false};
}

void DiffController::setGraphPresentation(ViewFrame* left, ViewFrame* right, bool active)
{
	if (!left || !right)
		return;
	pairFrames(left, right);
	// Any delayed activation or analysis-idle retry belongs to the graph widgets
	// from the previous presentation. Preserve the completed overlay itself: the
	// native Linear render layer consumes the same in-memory state.
	++m_activationGeneration;
	++m_selectedDiffRetryGeneration;
	if (!active)
		m_syncTimer->stop();
	else if (m_active && m_paneA && m_paneB
	    && ((m_paneA->frame() == left && m_paneB->frame() == right)
	        || (m_paneA->frame() == right && m_paneB->frame() == left)))
		m_syncTimer->start();
}

QString DiffController::computeFrame(ViewFrame* frame)
{
	DiffViewContainer* pane = paneForFrame(frame);
	return pane ? compute(pane) : QString("The Binja Diff pane is not ready yet.");
}

QString DiffController::prepareFunctionMatches(ViewFrame* frame)
{
	DiffEngine& engine = DiffEngine::instance();
	if (!engine.available())
		return QString("Diff engine unavailable: %1").arg(QString::fromStdString(engine.loadError()));
	DiffViewContainer* pane = paneForFrame(frame);
	if (!pane)
		return "The Binja Diff pane is not ready yet.";
	DiffViewContainer* partner = findPartner(pane);
	if (!partner)
		return "The paired Binja Diff pane is not ready yet.";
	auto cached = engine.cachedFunctionMatches(pane->data(), partner->data());
	if (cached && cached->valid)
		return "Function matches are ready.";
	beginFunctionMatch(pane, partner);
	return "Matching functions between binaries in the background…";
}

void DiffController::persistMatchesForFile(FileMetadata* file)
{
	if (!file)
		return;
	std::set<PendingMatchKey> visited;
	std::vector<std::pair<BinaryViewRef, BinaryViewRef>> pairs;
	for (const auto& [frame, partnerFrame] : m_explicitPartners)
	{
		DiffViewContainer* pane = paneForFrame(frame);
		DiffViewContainer* partner = paneForFrame(partnerFrame.frame);
		if (!pane || !partner || !pane->data() || !partner->data())
			continue;
		FileMetadata* paneFile = pane->data()->GetFile();
		FileMetadata* partnerFile = partner->data()->GetFile();
		if (!paneFile || !partnerFile
		    || (paneFile->GetSessionId() != file->GetSessionId()
		        && partnerFile->GetSessionId() != file->GetSessionId()))
			continue;
		auto key = canonicalViewPair(
		    reinterpret_cast<uintptr_t>(pane->data()->GetObject()),
		    reinterpret_cast<uintptr_t>(partner->data()->GetObject()));
		if (!visited.insert(key).second)
			continue;
		pairs.emplace_back(pane->data(), partner->data());
	}
	if (pairs.empty())
		return;
	// Persisting hashes each saved database in full and writes cache files, which
	// can take seconds for large BNDBs. Keep that work off the GUI thread.
	(void)QtConcurrent::run([pairs = std::move(pairs)]() {
		DiffEngine& engine = DiffEngine::instance();
		for (const auto& [a, b] : pairs)
		{
			try
			{
				// Pairs are deduplicated without direction; persist whichever
				// orientations are cached (each is a separate security assessment).
				engine.persistCachedFunctionMatches(a, b);
				engine.persistCachedFunctionMatches(b, a);
				engine.persistCachedBinaryExtraction(a);
				engine.persistCachedBinaryExtraction(b);
			}
			catch (const std::exception& exception)
			{
				LogWarn("binja_diff_view: could not refresh caches after save: %s", exception.what());
			}
		}
	});
}

QString DiffController::activateMatch(
    ViewFrame* left, ViewFrame* right, uint64_t addressA, uint64_t addressB)
{
	DiffViewContainer* paneA = paneForFrame(left);
	DiffViewContainer* paneB = paneForFrame(right);
	if (!paneA || !paneB)
		return "The paired graph panes are not available.";
	if (!paneIsCurrent(paneA) || !paneIsCurrent(paneB))
		return "Switch the workspace to Graph diff before opening an overlay.";
	pairFrames(left, right);
	// Navigation may report success before the DisassemblyView publishes its new
	// current function. Clear the old overlay and defer extraction until both
	// panes actually reach the requested pair.
	clear();
	uint64_t generation = ++m_activationGeneration;
	m_syncing = true;
	bool navigatedA = paneA->graphView()->navigate(addressA);
	bool navigatedB = paneB->graphView()->navigate(addressB);
	m_syncing = false;
	if (!navigatedA || !navigatedB)
		return "Could not navigate one of the panes to the selected match.";
	QTimer::singleShot(0, this,
	    [this, paneA = QPointer<DiffViewContainer>(paneA),
	        paneB = QPointer<DiffViewContainer>(paneB), addressA, addressB, generation]() {
		finishMatchActivation(paneA, paneB, addressA, addressB, generation, 40, false);
	    });
	return QString("Opening selected match 0x%1 ↔ 0x%2…")
	    .arg(addressA, 0, 16).arg(addressB, 0, 16);
}

void DiffController::finishMatchActivation(QPointer<DiffViewContainer> paneA,
    QPointer<DiffViewContainer> paneB, uint64_t addressA, uint64_t addressB,
    uint64_t generation, int attemptsRemaining, bool allowHiddenGraph)
{
	if (generation != m_activationGeneration || !paneA || !paneB
	    || !m_panes.count(paneA.data()) || !m_panes.count(paneB.data()))
		return;

	FunctionRef currentA = paneA->graphView()->getCurrentFunction();
	FunctionRef currentB = paneB->graphView()->getCurrentFunction();
	bool readyA = currentA && currentA->GetStart() == addressA;
	bool readyB = currentB && currentB->GetStart() == addressB;
	if (!readyA || !readyB)
	{
		if (attemptsRemaining <= 0)
		{
			QString status = "Timed out waiting for both graph panes to reach the selected match.";
			paneA->setStatus(status);
			paneB->setStatus(status);
			emit matchActivationFinished(paneA->frame(), paneB->frame(), status);
			return;
		}
		QTimer::singleShot(25, this,
		    [this, paneA, paneB, addressA, addressB, generation, attemptsRemaining,
		        allowHiddenGraph]() {
			finishMatchActivation(
			    paneA, paneB, addressA, addressB, generation, attemptsRemaining - 1,
			    allowHiddenGraph);
		    });
		return;
	}

	QString status = computeImpl(paneA.data(), allowHiddenGraph);
	paneA->setStatus(status);
	paneB->setStatus(status);
	emit matchActivationFinished(paneA->frame(), paneB->frame(), status);
}

QString DiffController::activateLinearMatch(
    ViewFrame* left, ViewFrame* right, uint64_t addressA, uint64_t addressB)
{
	DiffViewContainer* paneA = paneForFrame(left);
	DiffViewContainer* paneB = paneForFrame(right);
	if (!paneA || !paneB || !left || !right)
		return "The paired diff panes are not available.";
	View* leftView = left->getCurrentViewInterface();
	View* rightView = right->getCurrentViewInterface();
	if (!leftView || !rightView || left->getCurrentView().section(':', 0, 0) != "Linear"
	    || right->getCurrentView().section(':', 0, 0) != "Linear"
	    || paneIsCurrent(paneA) || paneIsCurrent(paneB))
		return "Switch both panes to Linear before opening a linear diff.";

	pairFrames(left, right);
	if (!DiffEngine::instance().cachedFunctionMatches(paneA->data(), paneB->data()))
	{
		uint64_t generation = ++m_activationGeneration;
		beginFunctionMatch(paneA, paneB);
		finishLinearMatchWhenReady(paneA, paneB, addressA, addressB, generation, 1200);
		return "Function matching is still running; the linear diff will open automatically…";
	}
	clear();
	FunctionViewType viewType = leftView->getILViewType();
	if (rightView->getILViewType() != viewType)
	{
		rightView->setILViewType(viewType);
		rightView->refreshContents();
	}
	paneA->graphView()->setILViewType(viewType);
	paneB->graphView()->setILViewType(viewType);
	uint64_t generation = ++m_activationGeneration;
	m_syncing = true;
	bool navigatedA = paneA->graphView()->navigate(addressA);
	bool navigatedB = paneB->graphView()->navigate(addressB);
	m_syncing = false;
	if (!navigatedA || !navigatedB)
		return "Could not prepare one of the functions for linear diffing.";
	QTimer::singleShot(0, this,
	    [this, paneA = QPointer<DiffViewContainer>(paneA),
	        paneB = QPointer<DiffViewContainer>(paneB), addressA, addressB, generation]() {
		finishMatchActivation(paneA, paneB, addressA, addressB, generation, 40, true);
	    });
	return QString("Preparing linear diff 0x%1 ↔ 0x%2…")
	    .arg(addressA, 0, 16).arg(addressB, 0, 16);
}

void DiffController::finishLinearMatchWhenReady(QPointer<DiffViewContainer> paneA,
    QPointer<DiffViewContainer> paneB, uint64_t addressA, uint64_t addressB,
    uint64_t generation, int attemptsRemaining)
{
	if (generation != m_activationGeneration || !paneA || !paneB
	    || !m_panes.count(paneA) || !m_panes.count(paneB))
		return;
	if (DiffEngine::instance().cachedFunctionMatches(paneA->data(), paneB->data()))
	{
		QString status = activateLinearMatch(
		    paneA->frame(), paneB->frame(), addressA, addressB);
		emit matchActivationFinished(paneA->frame(), paneB->frame(), status);
		return;
	}
	if (attemptsRemaining <= 0)
	{
		QString status = "Timed out waiting for function matching to finish.";
		emit matchActivationFinished(paneA->frame(), paneB->frame(), status);
		return;
	}
	QTimer::singleShot(250, this,
	    [this, paneA, paneB, addressA, addressB, generation, attemptsRemaining]() {
		finishLinearMatchWhenReady(
		    paneA, paneB, addressA, addressB, generation, attemptsRemaining - 1);
	    });
}

QString DiffController::navigateUnmatched(ViewFrame* frame, uint64_t address)
{
	DiffViewContainer* pane = paneForFrame(frame);
	if (!pane)
		return "The selected graph pane is not available.";
	if (!paneIsCurrent(pane))
		return "Switch the workspace to Graph diff before navigating its graph.";
	clear();
	m_syncing = true;
	bool navigated = pane->graphView()->navigate(address);
	m_syncing = false;
	return navigated ? QString("Showing unmatched function at 0x%1.").arg(address, 0, 16)
	                 : QString("Could not navigate to unmatched function at 0x%1.").arg(address, 0, 16);
}

FunctionMatchMapsRef DiffController::matchesForFrames(
    ViewFrame* left, ViewFrame* right) const
{
	DiffViewContainer* paneA = paneForFrame(left);
	DiffViewContainer* paneB = paneForFrame(right);
	if (!paneA || !paneB)
		return {};
	return DiffEngine::instance().cachedFunctionMatchesRef(paneA->data(), paneB->data());
}

std::map<uint64_t, std::string> DiffController::displayNamesForFrame(ViewFrame* frame) const
{
	std::map<uint64_t, std::string> names;
	DiffViewContainer* pane = paneForFrame(frame);
	if (!pane || !pane->data())
		return names;
	for (const auto& function : pane->data()->GetAnalysisFunctionList())
	{
		if (!function)
			continue;
		Ref<Symbol> symbol = function->GetSymbol();
		std::string name = functionDisplayName(function->GetView(), symbol);
		if (!name.empty())
			names.emplace(function->GetStart(), std::move(name));
	}
	return names;
}

SymbolPortPlan DiffController::planSymbolPort(ViewFrame* left, ViewFrame* right,
    const std::set<uint64_t>& selectedA, const SymbolPortOptions& options) const
{
	DiffViewContainer* paneA = paneForFrame(left);
	DiffViewContainer* paneB = paneForFrame(right);
	if (!paneA || !paneB || selectedA.empty())
		return {};
	auto maps = DiffEngine::instance().cachedFunctionMatches(paneA->data(), paneB->data());
	if (!maps || !maps->valid)
		return {};

	FunctionByAddress functionsA = functionsByAddress(paneA->data());
	FunctionByAddress functionsB = functionsByAddress(paneB->data());
	std::vector<SymbolPortCandidate> candidates;
	for (const auto& match : maps->matches)
	{
		if (!selectedA.count(match.a.address))
			continue;
		SymbolPortCandidate candidate;
		candidate.addressA = match.a.address;
		candidate.addressB = match.b.address;
		candidate.confidence = match.confidence;
		auto functionA = functionsA.find(match.a.address);
		auto functionB = functionsB.find(match.b.address);
		fillCandidateNames(candidate, true,
		    functionA == functionsA.end() ? FunctionRef() : functionA->second);
		fillCandidateNames(candidate, false,
		    functionB == functionsB.end() ? FunctionRef() : functionB->second);
		candidates.push_back(std::move(candidate));
	}
	return buildSymbolPortPlan(candidates, options);
}

SymbolPortPlan DiffController::planCurrentSymbolPort(ViewFrame* left, ViewFrame* right,
	const SymbolPortOptions& options) const
{
	DiffViewContainer* paneA = paneForFrame(left);
	DiffViewContainer* paneB = paneForFrame(right);
	if (!paneA || !paneB || !paneA->graphView() || !paneB->graphView())
		return {};
	FunctionRef functionA = displayedFunction(paneA);
	FunctionRef functionB = displayedFunction(paneB);
	if (!functionA || !functionB)
		return {};
	return planAddressPairSymbolPort(
	    left, right, functionA->GetStart(), functionB->GetStart(), options);
}

SymbolPortPlan DiffController::planAddressPairSymbolPort(ViewFrame* left, ViewFrame* right,
	uint64_t addressA, uint64_t addressB, const SymbolPortOptions& options) const
{
	DiffViewContainer* paneA = paneForFrame(left);
	DiffViewContainer* paneB = paneForFrame(right);
	if (!paneA || !paneB)
		return {};
	FunctionByAddress functionsA = functionsByAddress(paneA->data());
	FunctionByAddress functionsB = functionsByAddress(paneB->data());
	auto functionA = functionsA.find(addressA);
	auto functionB = functionsB.find(addressB);
	SymbolPortCandidate candidate;
	candidate.addressA = addressA;
	candidate.addressB = addressB;
	candidate.confidence = 1.0;  // the analyst explicitly chose this pair
	fillCandidateNames(candidate, true,
	    functionA == functionsA.end() ? FunctionRef() : functionA->second);
	fillCandidateNames(candidate, false,
	    functionB == functionsB.end() ? FunctionRef() : functionB->second);
	return buildSymbolPortPlan({candidate}, options);
}

bool DiffController::applySymbolPort(ViewFrame* left, ViewFrame* right,
    const SymbolPortPlan& plan, const SymbolPortOptions& options)
{
	DiffViewContainer* paneA = paneForFrame(left);
	DiffViewContainer* paneB = paneForFrame(right);
	if (!paneA || !paneB || plan.renames.empty() || m_symbolPortRunning)
		return false;
	auto cached = DiffEngine::instance().cachedFunctionMatchesRef(paneA->data(), paneB->data());
	if (!cached || !cached->valid)
		return false;

	BinaryViewRef a = paneA->data();
	BinaryViewRef b = paneB->data();
	BinaryViewRef target = options.direction == SymbolPortDirection::AToB ? b : a;
	FunctionMatchMaps maps = *cached;
	addManualMatchesForPlan(maps, plan, options.direction);
	Ref<BackgroundTask> task = new BackgroundTask("Binja Diff: porting symbols…", true);
	auto* watcher = new QFutureWatcher<SymbolPortApplyResult>(this);
	QPointer<DiffViewContainer> expectedA(paneA);
	QPointer<DiffViewContainer> expectedB(paneB);
	m_symbolPortRunning = true;
	m_symbolPortTarget.store(target->GetObject());

	connect(watcher, &QFutureWatcher<SymbolPortApplyResult>::finished, this,
	    [this, watcher, expectedA, expectedB, a, b, cached, maps, task]() mutable {
		SymbolPortApplyResult result;
		try
		{
			result = watcher->result();
		}
		catch (const std::exception& exception)
		{
			result.error = exception.what();
		}
		catch (...)
		{
			result.error = "unknown worker exception";
		}
		watcher->deleteLater();
		task->Finish();

		bool pairCacheCurrent = true;
		FunctionMatchMapsRef replacementSnapshot;
		if (result.success)
		{
			refreshMatchNames(maps, a, b);
			replacementSnapshot = DiffEngine::instance().replaceFunctionMatchesIfCurrentRef(
			    a, b, cached, maps);
			pairCacheCurrent = bool(replacementSnapshot);
			if (m_active && m_panes.count(m_paneA) && m_panes.count(m_paneB))
			{
				auto activeMaps = DiffEngine::instance().cachedFunctionMatchesRef(
				    m_paneA->data(), m_paneB->data());
				if (activeMaps)
					m_matchMaps = std::move(activeMaps);
				else
					clear();
			}
		}
		m_symbolPortTarget.store(nullptr);
		m_symbolPortRunning = false;

		if (expectedA && expectedB && result.success)
			emit functionMatchesUpdated(expectedA->frame(), expectedB->frame());
		QString message;
		if (!result.success)
			message = QString("Symbol port failed: %1")
			              .arg(QString::fromStdString(result.error.empty()
			                      ? "the undoable transaction was rejected" : result.error));
		else
		{
			message = QString("Ported %1 symbol(s) as one undo step.").arg(result.applied);
			if (result.changedAfterPreview)
				message += QString(" %1 target(s) changed after preview and were skipped.")
				               .arg(result.changedAfterPreview);
			if (result.cancelled)
				message += " The operation was cancelled; completed renames were kept in that undo step.";
			if (!pairCacheCurrent)
				message += " The binary changed during symbol port; rematch before using the diff.";
		}
		if (expectedA && expectedB)
			emit symbolPortFinished(expectedA->frame(), expectedB->frame(), result.success, message);
	});

	watcher->setFuture(QtConcurrent::run([target, plan, task]() {
		return applySymbolPortPlan(target, plan,
		    [task]() { return task->IsCancelled(); },
		    [task](size_t completed, size_t total) {
			    if ((completed - 1) % 64 == 0 || completed == total)
				    task->SetProgressText("Binja Diff: porting symbols ("
				        + std::to_string(completed) + "/" + std::to_string(total) + ")");
		    });
	}));
	return true;
}

DiffViewContainer* DiffController::findPartner(DiffViewContainer* src) const
{
	if (src->frame())
	{
		auto explicitPartner = m_explicitPartners.find(src->frame());
		if (explicitPartner != m_explicitPartners.end())
		{
			DiffViewContainer* pane = paneForFrame(explicitPartner->second.frame);
			if (pane && pane != src && pane->data() != src->data())
				return pane;
		}
	}

	// A normal in-file pane split duplicates the same BinaryView. That is useful
	// for comparing two locations in one binary, but it is not a binary diff.
	// Only pair panes backed by a different BinaryView; within those, prefer a
	// pane in the same sync group.
	SyncGroup* group = src->frame() ? src->frame()->syncGroup() : nullptr;
	DiffViewContainer* best = nullptr;
	int bestRank = -1;
	for (DiffViewContainer* other : m_panes)
	{
		if (other == src)
			continue;
		if (other->data() == src->data())
			continue;
		bool sameGroup = group && other->frame() && group->contains(other->frame());
		int rank = sameGroup ? 1 : 0;
		if (rank > bestRank)
		{
			bestRank = rank;
			best = other;
		}
	}
	return best;
}

bool DiffController::isCandidatePane(DiffViewContainer* src, DiffViewContainer* partner) const
{
	auto found = m_explicitPartners.find(src->frame());
	return found != m_explicitPartners.end() && !found->second.baseline
	    && found->second.frame == partner->frame();
}

DiffViewContainer* DiffController::paneForFrame(ViewFrame* frame) const
{
	if (!frame)
		return nullptr;
	for (DiffViewContainer* pane : m_panes)
		if (pane->frame() == frame)
			return pane;
	return nullptr;
}

void DiffController::onILViewTypeChange(
    ViewFrame* frame, View* changedView, const FunctionViewType& type)
{
	if (m_ilSyncing)
		return;
	DiffViewContainer* pane = paneForFrame(frame);
	if (!pane)
		return;
	// A cached hidden graph can emit an IL notification while Linear is current.
	// Only the visible view is allowed to drive presentation synchronization.
	if (changedView && frame && changedView != frame->getCurrentViewInterface())
		return;
	if (!paneIsCurrent(pane))
	{
		if (!frame || frame->getCurrentView().section(':', 0, 0) != "Linear")
			return;
		DiffViewContainer* partner = findPartner(pane);
		View* partnerView = partner && partner->frame()
		    ? partner->frame()->getCurrentViewInterface() : nullptr;
		m_ilSyncing = true;
		if (partnerView && partner->frame()->getCurrentView().section(':', 0, 0) == "Linear"
		    && partnerView->getILViewType() != type)
		{
			partnerView->setILViewType(type);
			partnerView->refreshContents();
		}
		bool recompute = m_active && m_paneA && m_paneB
		    && (pane == m_paneA || pane == m_paneB);
		QPointer<DiffViewContainer> leftPane = recompute ? m_paneA : nullptr;
		QPointer<DiffViewContainer> rightPane = recompute ? m_paneB : nullptr;
		uint64_t addressA = recompute && m_funcA && m_funcA->function
		    ? m_funcA->function->GetStart() : 0;
		uint64_t addressB = recompute && m_funcB && m_funcB->function
		    ? m_funcB->function->GetStart() : 0;
		m_ilSyncing = false;
		if (recompute)
			QTimer::singleShot(0, this, [this, leftPane, rightPane, addressA, addressB]() {
				if (leftPane && rightPane)
					activateLinearMatch(
					    leftPane->frame(), rightPane->frame(), addressA, addressB);
			});
		return;
	}

	m_ilSyncing = true;
	pane->setILLabel(ilDisplayName(type));
	emit ilViewTypeUpdated(frame, ilDisplayName(type));

	// Mirror the IL form onto the partner pane so both sides stay at the same level.
	DiffViewContainer* partner = (pane == m_paneA) ? m_paneB : (pane == m_paneB ? m_paneA : nullptr);
	if (!partner)
		partner = findPartner(pane);
	if (partner && m_panes.count(partner) && partner->graphView()->getILViewType() != type)
	{
		// Drive the DisassemblyView's own setILViewType (the form getILViewType
		// reads); the DisassemblyContainer overload does not move the view.
		partner->graphView()->setILViewType(type);
		partner->graphView()->refreshContents();
		partner->setILLabel(ilDisplayName(type));
		emit ilViewTypeUpdated(partner->frame(), ilDisplayName(type));
	}

	// Re-extract and re-overlay the diff at the new IL level. compute() itself
	// calls setILViewType on the partner, which re-enters this notification, so
	// the IL-sync guard must stay held across it to avoid unbounded recursion.
	if (m_active)
	{
		m_syncing = true;
		DiffViewContainer* recomputeFrom = (pane == m_paneB && m_panes.count(m_paneA)) ? m_paneA : pane;
		pane->setStatus(compute(recomputeFrom));
		m_syncing = false;
	}
	m_ilSyncing = false;
}

// Resolve a name-only counterpart only when the identity is unique on both
// sides. The old first-match fallback could silently open the wrong overload,
// thunk, or duplicate template instance.
static std::optional<uint64_t> functionByName(BinaryViewRef bv, FunctionRef func)
{
	if (!bv || !func)
		return std::nullopt;
	Ref<Symbol> sym = func->GetSymbol();
	if (!sym)
		return std::nullopt;
	std::string shortName = functionMatchName(func->GetView(), sym);
	std::string rawName = sym->GetRawName();
	if (isGeneratedFunctionName(shortName))
		shortName.clear();
	if (isGeneratedFunctionName(rawName))
		rawName.clear();
	if (shortName.empty() && rawName.empty())
		return std::nullopt;
	auto candidates = [&](BinaryViewRef view) {
		std::vector<FunctionRef> matches;
		if (!view)
			return matches;
		for (const auto& candidate : view->GetAnalysisFunctionList())
		{
			Ref<Symbol> candidateSymbol = candidate->GetSymbol();
			if (!candidateSymbol)
				continue;
			bool rawMatch = !rawName.empty()
			    && candidateSymbol->GetRawName() == rawName;
			bool shortMatch = !shortName.empty()
			    && functionMatchName(candidate->GetView(), candidateSymbol) == shortName;
			if (rawMatch || shortMatch)
				matches.push_back(candidate);
		}
		return matches;
	};

	BinaryViewRef source = func->GetView();
	auto sourceMatches = candidates(source);
	if (sourceMatches.size() != 1
	    || sourceMatches.front()->GetStart() != func->GetStart())
		return std::nullopt;
	auto targetMatches = candidates(bv);
	if (targetMatches.size() != 1)
		return std::nullopt;
	return targetMatches.front()->GetStart();
}

static std::optional<uint64_t> matchedCounterpart(
    const FunctionMatchMaps& maps, BinaryViewRef partnerBv, FunctionRef func,
    bool fromCandidate)
{
	if (!func)
		return std::nullopt;
	if (auto matched = counterpartAddress(maps.funcMap, func->GetStart(), fromCandidate))
		return matched;
	return functionByName(partnerBv, func);
}

void DiffController::beginFunctionMatch(DiffViewContainer* src, DiffViewContainer* partner)
{
	if (!src || !partner || !src->data() || !partner->data())
		return;
	// Matches are directional (A is the baseline), so only requests for the same
	// baseline/candidate orientation may share one worker.
	const bool fromCandidate = isCandidatePane(src, partner);
	BinaryViewRef a = fromCandidate ? partner->data() : src->data();
	BinaryViewRef b = fromCandidate ? src->data() : partner->data();
	const PendingMatchKey pendingKey = {
	    reinterpret_cast<uintptr_t>(a->GetObject()), reinterpret_cast<uintptr_t>(b->GetObject())};
	emit functionMatchingStarted(src->frame(), partner->frame());

	src->setBusy(true);
	partner->setBusy(true);
	src->setStatus("Matching functions between binaries in the background…");
	partner->setStatus("Matching functions between binaries in the background…");
	auto& waiters = m_pendingMatches[pendingKey];
	bool startWorker = waiters.empty();
	waiters.push_back({src, partner});
	if (!startWorker)
		return;
	if (!analysisIdle(src) || !analysisIdle(partner))
	{
		src->setStatus("Analysis is still updating; function matching will start automatically…");
		partner->setStatus("Analysis is still updating; function matching will start automatically…");
		retryFunctionMatchWhenAnalysisSettles(pendingKey, 0);
		return;
	}
	Ref<BackgroundTask> task = new BackgroundTask("Binja Diff: preparing function match…", true);
	auto* watcher = new QFutureWatcher<FunctionMatchMapsRef>(this);
	connect(watcher, &QFutureWatcher<FunctionMatchMapsRef>::finished, this,
	    [this, watcher, pendingKey, task]() {
		auto pending = m_pendingMatches.find(pendingKey);
		std::vector<PendingMatchWaiter> waiters;
		if (pending != m_pendingMatches.end())
		{
			waiters = std::move(pending->second);
			m_pendingMatches.erase(pending);
		}
		FunctionMatchMapsRef snapshot;
		FunctionMatchMaps failure;
		failure.valid = false;
		failure.error = "Function matching returned no result";
		try
		{
			snapshot = watcher->result();
		}
		catch (const std::exception& e)
		{
			failure.valid = false;
			failure.error = std::string("Function matching failed: ") + e.what();
		}
		catch (...)
		{
			failure.valid = false;
			failure.error = "Function matching failed with an unknown worker exception";
		}
		const FunctionMatchMaps& maps = snapshot ? *snapshot : failure;
		task->Finish();
		watcher->deleteLater();
		bool analysisChanged = !maps.valid
		    && maps.error.find("Analysis changed while function matching was running") != std::string::npos;
		if (analysisChanged)
		{
			auto& queued = m_pendingMatches[pendingKey];
			for (const auto& waiter : waiters)
			{
				if (!waiter.source || !waiter.partner)
					continue;
				waiter.source->setStatus(
				    "Analysis is still updating; function matching will retry automatically…");
				waiter.partner->setStatus(
				    "Analysis is still updating; function matching will retry automatically…");
				queued.push_back(waiter);
			}
			if (!queued.empty())
				retryFunctionMatchWhenAnalysisSettles(pendingKey, 0);
			return;
		}
		for (const auto& waiter : waiters)
		{
			if (waiter.source)
				waiter.source->setBusy(false);
			if (waiter.partner)
				waiter.partner->setBusy(false);
			if (!maps.valid)
			{
				QString error = QString::fromStdString(maps.error);
				if (waiter.source)
					waiter.source->setStatus(error);
				if (waiter.partner)
					waiter.partner->setStatus(error);
				if (waiter.source && waiter.partner)
					emit functionMatchingFailed(
					    waiter.source->frame(), waiter.partner->frame(), error);
				continue;
			}
			if (waiter.source && waiter.partner)
				emit functionMatchesUpdated(waiter.source->frame(), waiter.partner->frame());
			if (waiter.source && waiter.partner && m_panes.count(waiter.source)
			    && m_panes.count(waiter.partner) && findPartner(waiter.source) == waiter.partner
			    && paneIsCurrent(waiter.source) && paneIsCurrent(waiter.partner))
				waiter.source->setStatus(compute(waiter.source));
		}
	});
	watcher->setFuture(QtConcurrent::run([a, b, task]() -> FunctionMatchMapsRef {
		// Catch inside the worker. If an exception crosses QtConcurrent, Qt stores
		// only a sliced QUnhandledException and watcher->result() reports the
		// unhelpful text "std::exception".
		try
		{
			return DiffEngine::instance().matchFunctionsRef(a, b, task);
		}
		catch (const std::exception& exception)
		{
			FunctionMatchMaps failure;
			failure.valid = false;
			failure.error = std::string("Function matching failed: ") + exception.what();
			LogError("binja_diff_view: %s", failure.error.c_str());
			return std::make_shared<const FunctionMatchMaps>(std::move(failure));
		}
		catch (...)
		{
			FunctionMatchMaps failure;
			failure.valid = false;
			failure.error = "Function matching failed with an unknown worker exception";
			LogError("binja_diff_view: %s", failure.error.c_str());
			return std::make_shared<const FunctionMatchMaps>(std::move(failure));
		}
	}));
}

void DiffController::retryFunctionMatchWhenAnalysisSettles(
    const PendingMatchKey& key, int stableIdleChecks)
{
	QTimer::singleShot(250, this, [this, key, stableIdleChecks]() {
		auto pending = m_pendingMatches.find(key);
		if (pending == m_pendingMatches.end())
			return;
		auto first = std::find_if(pending->second.begin(), pending->second.end(),
		    [](const PendingMatchWaiter& waiter) { return waiter.source && waiter.partner; });
		if (first == pending->second.end())
		{
			m_pendingMatches.erase(pending);
			return;
		}
		bool idle = analysisIdle(first->source) && analysisIdle(first->partner);
		int nextStable = idle ? stableIdleChecks + 1 : 0;
		if (nextStable < 2)
		{
			retryFunctionMatchWhenAnalysisSettles(key, nextStable);
			return;
		}

		std::vector<PendingMatchWaiter> waiters = std::move(pending->second);
		m_pendingMatches.erase(pending);
		for (const auto& waiter : waiters)
			if (waiter.source && waiter.partner)
				beginFunctionMatch(waiter.source, waiter.partner);
	});
}

QString DiffController::recompute(DiffViewContainer* src)
{
	DiffViewContainer* partner = src ? findPartner(src) : nullptr;
	if (!src || !partner)
		return "No partner pane is available to recompute.";
	invalidateOverlayCache(src->data());
	invalidateOverlayCache(partner->data());
	DiffEngine::instance().invalidateMatchPair(src->data(), partner->data());
	return compute(src);
}

QString DiffController::compute(DiffViewContainer* src)
{
	return computeImpl(src, false);
}

QString DiffController::computeImpl(DiffViewContainer* src, bool allowHiddenGraph)
{
	++m_selectedDiffRetryGeneration;
	if (!src)
		return "The graph diff pane is not available.";
	DiffEngine& engine = DiffEngine::instance();
	if (!engine.available())
		return QString("Diff engine unavailable: %1").arg(QString::fromStdString(engine.loadError()));

	DiffViewContainer* partner = findPartner(src);
	if (!partner)
		return "No second Diff View pane for another binary found. Split the v1/v2 tabs into separate tab groups, then select Diff View in both.";
	if (!allowHiddenGraph && (!paneIsCurrent(src) || !paneIsCurrent(partner)))
		return "Graph diff is paused while the workspace is in Linear view.";

	FunctionRef funcA = displayedFunction(src);
	if (!funcA)
		return "This pane must be showing a function.";

	// The navigation driver may be either pane, but matching, rendering, and
	// deferred activation must retain the workspace's baseline/candidate order.
	const bool fromCandidate = isCandidatePane(src, partner);
	DiffViewContainer* baseline = fromCandidate ? partner : src;
	DiffViewContainer* candidate = fromCandidate ? src : partner;

	// Whole-binary matching is the expensive part. A cache miss is dispatched to
	// a worker; completion re-enters compute() on the GUI thread.
	auto cachedMaps = engine.cachedFunctionMatchesRef(baseline->data(), candidate->data());
	if (!cachedMaps)
	{
		if (m_active)
			clear();
		beginFunctionMatch(src, partner);
		return "Matching functions between binaries in the background…";
	}
	if (!cachedMaps->valid)
		return QString::fromStdString(cachedMaps->error);

	// Auto-pairing: navigate the partner pane to this function's counterpart.
	// Prefer the engine's address match; fall back to the same symbol name so a
	// function present in both binaries still pairs even when the matcher missed
	// it. Binary Ninja can acknowledge navigate() before the graph publishes its
	// new current function, so every automatic pairing uses the same deferred
	// activation path as a result-table row.
	std::optional<uint64_t> counterpart;
	counterpart = matchedCounterpart(*cachedMaps, partner->data(), funcA, fromCandidate);
	if (counterpart)
	{
		FunctionRef current = displayedFunction(partner);
		if (!current || current->GetStart() != *counterpart)
			return fromCandidate
			    ? activateMatch(baseline->frame(), candidate->frame(), *counterpart, funcA->GetStart())
			    : activateMatch(baseline->frame(), candidate->frame(), funcA->GetStart(), *counterpart);
	}
	return renderCurrentPair(baseline, candidate, std::move(cachedMaps), allowHiddenGraph);
}

QString DiffController::renderCurrentPair(DiffViewContainer* src, DiffViewContainer* partner,
	FunctionMatchMapsRef matchMaps, bool allowHiddenGraph)
{
	QElapsedTimer totalTimer;
	totalTimer.start();
	if (!src || !partner)
		return "The paired graph panes are not available.";
	FunctionRef funcA = displayedFunction(src);
	if (!funcA)
		return "This pane must be showing a function.";

	// A failed recompute must never leave an old overlay attached to newly
	// navigated panes. Clear the previous render state before extracting either view.
	clear();

	FunctionRef funcB = displayedFunction(partner);
	if (!funcB)
		return "The other pane must be showing a function.";

	auto cleanSavedView = [](DiffViewContainer* pane) {
		FileMetadata* file = pane && pane->data() ? pane->data()->GetFile() : nullptr;
		return file && !file->IsModified() && !file->IsAnalysisChanged();
	};
	bool renderPreparedSnapshot = matchMaps->restoredFromDisk
	    && cleanSavedView(src) && cleanSavedView(partner);
	if ((!analysisIdle(src) || !analysisIdle(partner)) && !renderPreparedSnapshot)
	{
		uint64_t generation = ++m_selectedDiffRetryGeneration;
		retrySelectedDiffWhenAnalysisSettles(
		    src, partner, generation, 0, allowHiddenGraph);
		QString status = "Analysis is still updating; the selected graph diff will retry automatically…";
		src->setStatus(status);
		partner->setStatus(status);
		return status;
	}

	// Diff like-for-like: drive both panes at the source pane's IL level. Use the
	// DisassemblyView's setILViewType (not the container overload, which does not
	// move the view), so the partner actually renders the same IL form.
	FunctionViewType viewType = src->graphView()->getILViewType();
	partner->graphView()->setILViewType(viewType);

	QElapsedTimer phaseTimer;
	phaseTimer.start();
	OverlayCacheKey cacheKey {
	    reinterpret_cast<uintptr_t>(src->data()->GetObject()),
	    reinterpret_cast<uintptr_t>(partner->data()->GetObject()),
	    funcA->GetStart(), funcB->GetStart(), static_cast<int>(viewType.type),
	    viewType.name, matchMaps.get()};
	std::shared_ptr<const ExtractedFunction> extractedA, extractedB;
	std::shared_ptr<const BlockDiffResult> result;
	bool overlayCacheHit = false;
	if (auto cached = cachedOverlay(cacheKey))
	{
		extractedA = cached->functionA;
		extractedB = cached->functionB;
		result = cached->result;
		overlayCacheHit = true;
	}
	qint64 extractionMs = phaseTimer.elapsed();
	qint64 diffMs = 0;
	if (!overlayCacheHit)
	{
		// Capture only core references and immutable data for the worker. Native
		// graph widgets, navigation, and overlay application stay on this thread.
		struct PreparedOverlay
		{
			OverlayCacheEntry entry;
			QString error;
			qint64 extractionMs = 0, diffMs = 0;
		};
		const uint64_t generation = m_selectedDiffRetryGeneration;
		const uint64_t revision = m_overlayRevision.load();
		auto cancelled = std::make_shared<std::atomic<bool>>(false);
		m_selectedDiffCancelled = cancelled;
		QPointer<DiffViewContainer> source = src, target = partner;
		Ref<BackgroundTask> task = new BackgroundTask("Binja Diff: extracting selected functions…", true);
		auto* watcher = new QFutureWatcher<PreparedOverlay>(this);
		connect(watcher, &QFutureWatcher<PreparedOverlay>::finished, this,
		    [this, watcher, source, target, generation, revision, cancelled, task,
		        overlayCacheKey = cacheKey, allowHiddenGraph]() {
			PreparedOverlay prepared;
			try { prepared = watcher->result(); }
			catch (const std::exception& e) { prepared.error = QString::fromUtf8(e.what()); }
			catch (...) { prepared.error = "Selected function diff failed."; }
			task->Finish();
			watcher->deleteLater();
			if (generation != m_selectedDiffRetryGeneration || cancelled->load()
			    || !source || !target || !m_panes.count(source) || !m_panes.count(target)
			    || findPartner(source) != target)
				return;
			FunctionRef currentA = displayedFunction(source), currentB = displayedFunction(target);
			if (!currentA || !currentB || currentA->GetStart() != overlayCacheKey.functionA
			    || currentB->GetStart() != overlayCacheKey.functionB)
				return;
			auto typeA = source->graphView()->getILViewType();
			auto typeB = target->graphView()->getILViewType();
			if (static_cast<int>(typeA.type) != overlayCacheKey.viewType
			    || typeA.name != overlayCacheKey.viewTypeName
			    || typeA.type != typeB.type || typeA.name != typeB.name
			    || (!allowHiddenGraph && (!paneIsCurrent(source) || !paneIsCurrent(target))))
				return;
			QString status;
			auto completedMaps = prepared.entry.matchMaps;
			if (task->IsCancelled())
				status = "Selected function diff cancelled.";
			else if (!prepared.error.isEmpty())
				status = prepared.error;
			else if (!cacheOverlay(std::move(prepared.entry), revision))
			{
				status = "Analysis changed; retrying selected function diff…";
				retrySelectedDiffWhenAnalysisSettles(source, target, generation, 0, allowHiddenGraph);
			}
			else
			{
				LogInfo("binja_diff_view: background selected diff extraction=%lld ms diff=%lld ms",
				    static_cast<long long>(prepared.extractionMs), static_cast<long long>(prepared.diffMs));
				status = renderCurrentPair(source, target, std::move(completedMaps), allowHiddenGraph);
			}
			source->setStatus(status);
			target->setStatus(status);
			emit matchActivationFinished(source->frame(), target->frame(), status);
		});
		watcher->setFuture(QtConcurrent::run([funcA, funcB, viewType, cacheKey, matchMaps, cancelled, task]() {
			PreparedOverlay prepared;
			try
			{
				auto stopped = [&]() { return cancelled->load() || task->IsCancelled(); };
				if (stopped()) return prepared;
				QElapsedTimer timer;
				timer.start();
				auto a = DiffEngine::instance().extract(funcA, viewType);
				if (stopped()) return prepared;
				auto b = DiffEngine::instance().extract(funcB, viewType);
				prepared.extractionMs = timer.elapsed();
				if (stopped()) return prepared;
				if (!a || !b)
				{
					prepared.error = "Could not extract function graphs; analysis may still be running.";
					return prepared;
				}
				task->SetProgressText("Binja Diff: comparing selected functions…");
				timer.restart();
				auto result = DiffEngine::instance().blockDiff(*a, *b, *matchMaps);
				prepared.diffMs = timer.elapsed();
				if (stopped()) return prepared;
				if (!result) { prepared.error = "Engine block diff failed (see log)."; return prepared; }
				// Any compatibility fallback must also stay off the event thread.
				if (!result->ilSimilarity)
					result->ilSimilarity = DiffEngine::instance().ilSimilarity(*a, *b, *matchMaps);
				prepared.entry = {cacheKey, matchMaps,
				    std::make_shared<const ExtractedFunction>(std::move(*a)),
				    std::make_shared<const ExtractedFunction>(std::move(*b)),
				    std::make_shared<const BlockDiffResult>(std::move(*result))};
			}
			catch (const std::exception& e) { prepared.error = QString::fromUtf8(e.what()); }
			catch (...) { prepared.error = "Selected function diff failed with an unknown exception."; }
			return prepared;
		}));
		QString status = "Preparing selected function diff in the background… (cancel in Background Tasks)";
		src->setStatus(status);
		partner->setStatus(status);
		return status;
	}

	m_paneA = src;
	m_paneB = partner;
	m_matchMaps = std::move(matchMaps);
	m_funcA = std::move(extractedA);
	m_funcB = std::move(extractedB);
	for (const auto& p : result->pairs)
	{
		if (p.a >= m_funcA->blocks.size() || p.b >= m_funcB->blocks.size())
			continue;
		m_aToB[p.a] = p.b;
		m_bToA[p.b] = p.a;
	}
	phaseTimer.restart();
	applyOverlay(*result);
	qint64 overlayMs = phaseTimer.elapsed();
	m_active = true;
	m_changeCursor.reset();
	m_lastTypeA = src->graphView()->getILViewType();
	m_lastTypeB = partner->graphView()->getILViewType();
	m_lastOffsetA = src->graphView()->getCurrentOffset();
	m_lastOffsetB = partner->graphView()->getCurrentOffset();
	if (allowHiddenGraph)
		m_syncTimer->stop();
	else
		m_syncTimer->start();

	src->graphView()->refreshContents();
	partner->graphView()->refreshContents();
	if (View* current = src->frame()->getCurrentViewInterface(); current != src->getView())
		current->refreshContents();
	if (View* current = partner->frame()->getCurrentViewInterface(); current != partner->getView())
		current->refreshContents();

	size_t changed = 0;
	for (const auto& p : result->pairs)
		if (p.status == BlockStatus::Changed)
			changed++;

	// The block endpoint returns the whole-function semantic score with its
	// correspondence result. Fall back only when paired with an older engine.
	std::optional<double> ilSim = result->ilSimilarity;

	QString ilText = ilSim ? QString::number(*ilSim, 'f', 2) : QString("n/a");
	auto functionLabel = [](FunctionRef func) {
		Ref<Symbol> symbol = func ? func->GetSymbol() : nullptr;
		std::string name = functionDisplayName(func ? func->GetView() : nullptr, symbol);
		if (!name.empty())
			return QString::fromStdString(name);
		return func ? QString("sub_%1").arg(func->GetStart(), 0, 16) : QString("unknown");
	};
	QString status = QString("%1 ↔ %2 · IL similarity %3 · block similarity %4 · %5 changed, %6 removed, %7 added · %8 fn matches")
	        .arg(functionLabel(funcA))
	        .arg(functionLabel(funcB))
	        .arg(ilText)
	        .arg(result->similarity, 0, 'f', 2)
	        .arg(changed)
	        .arg(result->onlyA.size())
	        .arg(result->onlyB.size())
	        .arg(m_matchMaps->matchCount);
	// Short, always-visible similarity indicator on both panes (the score is
	// otherwise pushed off-screen by the long function names in the status text).
	QString simLabel = QString("IL %1 · block %2").arg(ilText).arg(result->similarity, 0, 'f', 2);
	QString details = QString("Blocks: %1 changed, %2 removed, %3 added")
	    .arg(changed).arg(result->onlyA.size()).arg(result->onlyB.size());
	// Publish both panes here, including cache hits reached via computeFrame().
	// Those callers consume the return value in a footer, not the source pane.
	src->setDiffSummary(status, details, simLabel);
	partner->setDiffSummary(status, details, simLabel);
	src->setDiffActive(true);
	partner->setDiffActive(true);
	LogInfo("binja_diff_view: selected graph timing cache=%s extraction=%lld ms diff=%lld ms "
	        "overlay=%lld ms total=%lld ms (%zu/%zu blocks, %zu changed line diffs)",
	    overlayCacheHit ? "hit" : "miss",
	    static_cast<long long>(extractionMs), static_cast<long long>(diffMs),
	    static_cast<long long>(overlayMs), static_cast<long long>(totalTimer.elapsed()),
	    m_funcA->blocks.size(), m_funcB->blocks.size(), result->lineDiffs.size());
	return status;
}

void DiffController::retrySelectedDiffWhenAnalysisSettles(
    QPointer<DiffViewContainer> source, QPointer<DiffViewContainer> partner,
    uint64_t generation, int stableIdleChecks, bool allowHiddenGraph)
{
	QTimer::singleShot(150, this,
	    [this, source, partner, generation, stableIdleChecks, allowHiddenGraph]() {
		if (generation != m_selectedDiffRetryGeneration || !source || !partner
		    || !m_panes.count(source) || !m_panes.count(partner)
		    || (!allowHiddenGraph && (!paneIsCurrent(source) || !paneIsCurrent(partner))))
			return;
		bool idle = analysisIdle(source) && analysisIdle(partner);
		int nextStable = idle ? stableIdleChecks + 1 : 0;
		if (nextStable < 2)
		{
			retrySelectedDiffWhenAnalysisSettles(
			    source, partner, generation, nextStable, allowHiddenGraph);
			return;
		}
		source->setStatus(computeImpl(source, allowHiddenGraph));
	});
}

void DiffController::highlightBlock(const ExtractedFunction& f, const ExtractedBlock& block,
    const BNHighlightColor& color)
{
	if (!f.function || !f.function->GetView())
		return;
	OverlayBlockKey key {reinterpret_cast<uintptr_t>(f.function->GetView()->GetObject()),
	    f.function->GetStart(), block.firstAddr};
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	m_blockColors[key] = color;
	for (size_t i = 0; i < block.lineAddrs.size(); i++)
	{
		size_t instructionIndex = i < block.lineInstrIndices.size()
		    ? block.lineInstrIndices[i] : i;
		m_linearLineColors[{key.viewIdentity, key.functionStart,
		    block.lineAddrs[i], instructionIndex}] = color;
		auto fallbackKey = std::make_tuple(key.viewIdentity, key.functionStart, instructionIndex);
		auto fallback = m_linearColorsByInstruction.find(fallbackKey);
		if (fallback == m_linearColorsByInstruction.end()
		    || block.lineAddrs[i] <= fallback->second.first)
			m_linearColorsByInstruction[fallbackKey] = {block.lineAddrs[i], color};
	}
}

void DiffController::highlightLine(const ExtractedFunction& f, const ExtractedBlock& block,
    uint64_t addr, size_t instructionIndex, const BNHighlightColor& color)
{
	if (!f.function || !f.function->GetView())
		return;
	OverlayLineKey key {reinterpret_cast<uintptr_t>(f.function->GetView()->GetObject()),
	    f.function->GetStart(), block.firstAddr, addr, instructionIndex};
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	m_lineColors[key] = color;
	m_linearLineColors[{key.viewIdentity, key.functionStart, addr, instructionIndex}] = color;
	auto fallbackKey = std::make_tuple(key.viewIdentity, key.functionStart, instructionIndex);
	auto fallback = m_linearColorsByInstruction.find(fallbackKey);
	if (fallback == m_linearColorsByInstruction.end() || addr <= fallback->second.first)
		m_linearColorsByInstruction[fallbackKey] = {addr, color};
}

void DiffController::applyOverlay(const BlockDiffResult& result)
{
	if (!m_funcA || !m_funcB)
		return;
	const ExtractedFunction& funcA = *m_funcA;
	const ExtractedFunction& funcB = *m_funcB;
	DiffEngine& engine = DiffEngine::instance();
	m_changes.clear();

	for (const auto& p : result.pairs)
	{
		if (p.a >= funcA.blocks.size() || p.b >= funcB.blocks.size())
			continue;
		const ExtractedBlock& blockA = funcA.blocks[p.a];
		const ExtractedBlock& blockB = funcB.blocks[p.b];
		switch (p.status)
		{
		case BlockStatus::Equal:
			break;
		case BlockStatus::Cosmetic:
			highlightBlock(funcA, blockA, diffPalette().cosmetic);
			highlightBlock(funcB, blockB, diffPalette().cosmetic);
			break;
		case BlockStatus::Changed:
		{
			highlightBlock(funcA, blockA, diffPalette().changed);
			highlightBlock(funcB, blockB, diffPalette().changed);
			m_changes.push_back({blockA.firstAddr, blockB.firstAddr});

			// The block endpoint normally includes line/token detail for every
			// changed pair. Retain the legacy call only as an ABI fallback.
			std::optional<LineDiff> fallbackLines;
			const LineDiff* lines = nullptr;
			auto preparedLines = result.lineDiffs.find({p.a, p.b});
			if (preparedLines != result.lineDiffs.end())
				lines = &preparedLines->second;
			else
			{
				fallbackLines = engine.lineDiff(blockA, blockB, *m_matchMaps);
				if (fallbackLines)
					lines = &*fallbackLines;
			}
			if (!lines)
				break;
			auto spanEmphasis = [](const std::vector<TokenSpan>& spans) {
				LineEmphasis e;
				e.spans = spans;
				return e;
			};
			OverlayBlockKey keyA {reinterpret_cast<uintptr_t>(funcA.function->GetView()->GetObject()),
			    funcA.function->GetStart(), blockA.firstAddr};
			OverlayBlockKey keyB {reinterpret_cast<uintptr_t>(funcB.function->GetView()->GetObject()),
			    funcB.function->GetStart(), blockB.firstAddr};
			size_t ia = 0, ib = 0;
			for (const DiffLine& l : lines->lines)
			{
				switch (l.op)
				{
				case LineOp::Equal:
					ia++;
					ib++;
					break;
				case LineOp::Rename:
				case LineOp::Replace:
				{
					bool replace = l.op == LineOp::Replace;
					if (ia < blockA.lineAddrs.size())
						highlightLine(funcA, blockA, blockA.lineAddrs[ia],
						    ia < blockA.lineInstrIndices.size() ? blockA.lineInstrIndices[ia] : ia,
						    replace ? diffPalette().removed : diffPalette().cosmetic);
					if (ib < blockB.lineAddrs.size())
						highlightLine(funcB, blockB, blockB.lineAddrs[ib],
						    ib < blockB.lineInstrIndices.size() ? blockB.lineInstrIndices[ib] : ib,
						    replace ? diffPalette().added : diffPalette().cosmetic);
					if (replace)
					{
						LineEmphasis ea = spanEmphasis(l.aSpans);
						if (std::any_of(ea.spans.begin(), ea.spans.end(),
						        [](const TokenSpan& span) { return span.changed; }))
						{
							ea.lineAddress = ia < blockA.lineAddrs.size() ? blockA.lineAddrs[ia] : 0;
							ea.instructionIndex = ia < blockA.lineInstrIndices.size()
							    ? blockA.lineInstrIndices[ia] : ia;
							std::lock_guard<std::mutex> lock(m_overlayMutex);
							m_linearEmphasis[{keyA.viewIdentity, keyA.functionStart,
							    ea.lineAddress, ea.instructionIndex}] = ea;
							m_lineEmphasis[{keyA.viewIdentity, keyA.functionStart, keyA.blockStart,
							    ea.lineAddress, ea.instructionIndex}] = ea;
							auto fallbackKey = std::make_tuple(
							    keyA.viewIdentity, keyA.functionStart, ea.instructionIndex);
							auto fallback = m_linearEmphasisByInstruction.find(fallbackKey);
							if (fallback == m_linearEmphasisByInstruction.end()
							    || ea.lineAddress <= fallback->second.first)
								m_linearEmphasisByInstruction[fallbackKey] = {ea.lineAddress, ea};
						}
						LineEmphasis eb = spanEmphasis(l.bSpans);
						if (std::any_of(eb.spans.begin(), eb.spans.end(),
						        [](const TokenSpan& span) { return span.changed; }))
						{
							eb.lineAddress = ib < blockB.lineAddrs.size() ? blockB.lineAddrs[ib] : 0;
							eb.instructionIndex = ib < blockB.lineInstrIndices.size()
							    ? blockB.lineInstrIndices[ib] : ib;
							std::lock_guard<std::mutex> lock(m_overlayMutex);
							m_linearEmphasis[{keyB.viewIdentity, keyB.functionStart,
							    eb.lineAddress, eb.instructionIndex}] = eb;
							m_lineEmphasis[{keyB.viewIdentity, keyB.functionStart, keyB.blockStart,
							    eb.lineAddress, eb.instructionIndex}] = eb;
							auto fallbackKey = std::make_tuple(
							    keyB.viewIdentity, keyB.functionStart, eb.instructionIndex);
							auto fallback = m_linearEmphasisByInstruction.find(fallbackKey);
							if (fallback == m_linearEmphasisByInstruction.end()
							    || eb.lineAddress <= fallback->second.first)
								m_linearEmphasisByInstruction[fallbackKey] = {eb.lineAddress, eb};
						}
					}
					ia++;
					ib++;
					break;
				}
				case LineOp::Delete:
					if (ia < blockA.lineAddrs.size())
						highlightLine(
						    funcA, blockA, blockA.lineAddrs[ia],
						    ia < blockA.lineInstrIndices.size() ? blockA.lineInstrIndices[ia] : ia,
						    diffPalette().removed);
					ia++;
					break;
				case LineOp::Insert:
					if (ib < blockB.lineAddrs.size())
						highlightLine(
						    funcB, blockB, blockB.lineAddrs[ib],
						    ib < blockB.lineInstrIndices.size() ? blockB.lineInstrIndices[ib] : ib,
						    diffPalette().added);
					ib++;
					break;
				}
			}
			break;
		}
		}
	}
	for (size_t i : result.onlyA)
	{
		if (i >= funcA.blocks.size())
			continue;
		highlightBlock(funcA, funcA.blocks[i], diffPalette().removed);
		m_changes.push_back({funcA.blocks[i].firstAddr, std::nullopt});
	}
	for (size_t i : result.onlyB)
	{
		if (i >= funcB.blocks.size())
			continue;
		highlightBlock(funcB, funcB.blocks[i], diffPalette().added);
		m_changes.push_back({std::nullopt, funcB.blocks[i].firstAddr});
	}

	std::sort(m_changes.begin(), m_changes.end(), [](const ChangeSite& x, const ChangeSite& y) {
		return x.addrA.value_or(x.addrB.value_or(0)) < y.addrA.value_or(y.addrB.value_or(0));
	});
}

std::optional<uint64_t> DiffController::overlayBlockStart(
    BinaryView* view, uint64_t funcStart, const std::vector<DisassemblyTextLine>& lines)
{
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	const uintptr_t identity = reinterpret_cast<uintptr_t>(view ? view->GetObject() : nullptr);
	// UI settings can prepend function labels or comments that are absent from
	// extraction. Resolve the stored block key from its instruction lines rather
	// than assuming that the first displayed line is the first extracted line.
	for (const auto& line : lines)
	{
		if (m_blockColors.find({identity, funcStart, line.addr}) != m_blockColors.end())
			return line.addr;
	}
	return std::nullopt;
}

std::optional<BNHighlightColor> DiffController::colorForBlock(
    BinaryView* view, uint64_t funcStart, uint64_t firstLineAddr)
{
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	auto it = m_blockColors.find({reinterpret_cast<uintptr_t>(view ? view->GetObject() : nullptr),
	    funcStart, firstLineAddr});
	return it == m_blockColors.end() ? std::nullopt : std::optional<BNHighlightColor>(it->second);
}

std::optional<BNHighlightColor> DiffController::colorForLine(
    BinaryView* view, uint64_t funcStart, uint64_t firstLineAddr, uint64_t lineAddr,
    size_t instructionIndex)
{
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	auto it = m_lineColors.find({reinterpret_cast<uintptr_t>(view ? view->GetObject() : nullptr),
	    funcStart, firstLineAddr, lineAddr, instructionIndex});
	return it == m_lineColors.end() ? std::nullopt : std::optional<BNHighlightColor>(it->second);
}

std::optional<LineEmphasis> DiffController::emphasisForLine(
    BinaryView* view, uint64_t funcStart, uint64_t firstLineAddr, uint64_t lineAddr,
    size_t instructionIndex)
{
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	auto it = m_lineEmphasis.find({reinterpret_cast<uintptr_t>(view ? view->GetObject() : nullptr),
	    funcStart, firstLineAddr, lineAddr, instructionIndex});
	return it == m_lineEmphasis.end() ? std::nullopt : std::optional<LineEmphasis>(it->second);
}

std::optional<BNHighlightColor> DiffController::colorForLinearLine(
    BinaryView* view, uint64_t funcStart, uint64_t lineAddr, size_t instructionIndex)
{
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	uintptr_t identity = reinterpret_cast<uintptr_t>(view ? view->GetObject() : nullptr);
	auto exact = m_linearLineColors.find(
	    {identity, funcStart, lineAddr, instructionIndex});
	if (exact != m_linearLineColors.end())
		return exact->second;
	// HLIL body formatting can move an expression to a synthetic address while
	// retaining its stable instruction index. Use the precomputed fallback index.
	if (instructionIndex != std::numeric_limits<size_t>::max())
	{
		auto fallback = m_linearColorsByInstruction.find(
		    std::make_tuple(identity, funcStart, instructionIndex));
		if (fallback != m_linearColorsByInstruction.end())
			return fallback->second.second;
	}
	return std::nullopt;
}

std::optional<LineEmphasis> DiffController::emphasisForLinearLine(
    BinaryView* view, uint64_t funcStart, uint64_t lineAddr, size_t instructionIndex)
{
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	uintptr_t identity = reinterpret_cast<uintptr_t>(view ? view->GetObject() : nullptr);
	auto exact = m_linearEmphasis.find({identity, funcStart, lineAddr, instructionIndex});
	if (exact != m_linearEmphasis.end())
		return exact->second;
	if (instructionIndex != std::numeric_limits<size_t>::max())
	{
		auto fallback = m_linearEmphasisByInstruction.find(
		    std::make_tuple(identity, funcStart, instructionIndex));
		if (fallback != m_linearEmphasisByInstruction.end())
			return fallback->second.second;
	}
	return std::nullopt;
}

void DiffController::clear()
{
	if (m_selectedDiffCancelled)
		m_selectedDiffCancelled->store(true);
	m_selectedDiffCancelled.reset();
	// Cancel any deferred result-row activation, including one waiting for a
	// DisassemblyView navigation to publish its new current function.
	++m_activationGeneration;
	++m_selectedDiffRetryGeneration;
	m_syncTimer->stop();
	{
		std::lock_guard<std::mutex> lock(m_overlayMutex);
		m_blockColors.clear();
		m_lineColors.clear();
		m_lineEmphasis.clear();
		m_linearEmphasis.clear();
		m_linearLineColors.clear();
		m_linearEmphasisByInstruction.clear();
		m_linearColorsByInstruction.clear();
	}
	m_changes.clear();
	m_aToB.clear();
	m_bToA.clear();
	// Preliminary cold-start overlays intentionally keep navigation inactive,
	// but they still own pane render state that Clear or a completed match must
	// remove.
	if (m_paneA || m_paneB)
	{
		if (m_panes.count(m_paneA))
		{
			m_paneA->setSimilarityLabel("");
			m_paneA->setDiffActive(false);
			m_paneA->graphView()->refreshContents();
			if (View* current = m_paneA->frame()->getCurrentViewInterface();
			    current != m_paneA->getView())
				current->refreshContents();
		}
		if (m_panes.count(m_paneB))
		{
			m_paneB->setSimilarityLabel("");
			m_paneB->setDiffActive(false);
			m_paneB->graphView()->refreshContents();
			if (View* current = m_paneB->frame()->getCurrentViewInterface();
			    current != m_paneB->getView())
				current->refreshContents();
		}
	}
	m_active = false;
	m_paneA = m_paneB = nullptr;
	m_lastDriver = nullptr;
	m_funcA.reset();
	m_funcB.reset();
	m_matchMaps.reset();
	m_changeCursor.reset();
}

void DiffController::jumpToChange(DiffViewContainer* src, bool forward)
{
	if (!m_active || m_changes.empty())
		return;
	size_t cursor = advanceChangeCursor(m_changeCursor, m_changes.size(), forward);
	const ChangeSite& site = m_changes[cursor];

	m_syncing = true;
	if (site.addrA && m_panes.count(m_paneA))
		m_paneA->graphView()->showAddress(*site.addrA, true, true);
	if (site.addrB && m_panes.count(m_paneB))
		m_paneB->graphView()->showAddress(*site.addrB, true, true);
	if (m_panes.count(m_paneA))
		m_lastOffsetA = m_paneA->graphView()->getCurrentOffset();
	if (m_panes.count(m_paneB))
		m_lastOffsetB = m_paneB->graphView()->getCurrentOffset();
	m_syncing = false;

	QString label = QString("Change %1/%2").arg(cursor + 1).arg(m_changes.size());
	DiffViewContainer* labelTarget = src ? src : m_paneA;
	if (m_panes.count(labelTarget))
		labelTarget->setStatus(label);
}

std::optional<size_t> DiffController::blockAtOffset(
    const ExtractedFunction& f, uint64_t offset) const
{
	for (const auto& block : f.blocks)
	{
		for (uint64_t addr : block.lineAddrs)
			if (addr == offset)
				return block.index;
	}
	for (const auto& block : f.blocks)
	{
		if (!block.basicBlock)
			continue;
		uint64_t start = block.basicBlock->GetStart();
		uint64_t end = block.basicBlock->GetEnd();
		if (offset >= start && offset < end)
			return block.index;
	}
	return std::nullopt;
}

void DiffController::syncTimerTick()
{
	if (!m_active || m_syncing)
		return;
	if (!m_panes.count(m_paneA) || !m_panes.count(m_paneB))
		return;
	if (!paneIsCurrent(m_paneA) || !paneIsCurrent(m_paneB))
		return;

	// Function-level coupling: if either pane moves to another function, treat
	// that pane as the driver, find the matched function in the other binary,
	// and recompute the block/line correspondence for the new side-by-side pair.
	FunctionRef currentFuncA = m_paneA->graphView()->getCurrentFunction();
	FunctionRef currentFuncB = m_paneB->graphView()->getCurrentFunction();
	uint64_t currentStartA = currentFuncA ? currentFuncA->GetStart() : 0;
	uint64_t currentStartB = currentFuncB ? currentFuncB->GetStart() : 0;
	uint64_t activeStartA = m_funcA && m_funcA->function ? m_funcA->function->GetStart() : 0;
	uint64_t activeStartB = m_funcB && m_funcB->function ? m_funcB->function->GetStart() : 0;
	bool movedFuncA = currentStartA && currentStartA != activeStartA;
	bool movedFuncB = currentStartB && currentStartB != activeStartB;
	if (movedFuncA && (!movedFuncB || m_lastDriver != m_paneB))
	{
		m_syncing = true;
		m_paneA->setStatus(compute(m_paneA));
		m_syncing = false;
		return;
	}
	if (movedFuncB)
	{
		m_syncing = true;
		m_paneB->setStatus(compute(m_paneB));
		m_syncing = false;
		return;
	}

	// IL level coupling: when one pane's level changes, recompute the diff at
	// the new level from that pane (which also switches the partner to match).
	FunctionViewType typeA = m_paneA->graphView()->getILViewType();
	FunctionViewType typeB = m_paneB->graphView()->getILViewType();
	if (typeA != m_lastTypeA || typeB != m_lastTypeB)
	{
		DiffViewContainer* changedPane = (typeA != m_lastTypeA) ? m_paneA : m_paneB;
		m_lastTypeA = typeA;
		m_lastTypeB = typeB;
		if (typeA != typeB)
		{
			m_syncing = true;
			DiffViewContainer* recomputeFrom =
			    (changedPane == m_paneB && m_panes.count(m_paneA)) ? m_paneA : changedPane;
			changedPane->setStatus(compute(recomputeFrom));
			m_syncing = false;
			return;
		}
	}

	// Follow whichever pane the user moved; mirror its block's counterpart.
	uint64_t offsetA = m_paneA->graphView()->getCurrentOffset();
	uint64_t offsetB = m_paneB->graphView()->getCurrentOffset();
	bool movedOffsetA = offsetA != m_lastOffsetA;
	bool movedOffsetB = offsetB != m_lastOffsetB;

	if (movedOffsetA && (!movedOffsetB || m_lastDriver != m_paneB))
		mirrorBlock(*m_funcA, offsetA, m_aToB, *m_funcB, m_paneB);
	else if (movedOffsetB)
		mirrorBlock(*m_funcB, offsetB, m_bToA, *m_funcA, m_paneA);

	// Re-read after mirroring. navigate() can synchronously move the partner; if
	// we kept the pre-navigation value, the next tick would treat the mirrored
	// pane as the driver and bounce the view back.
	m_lastOffsetA = m_paneA->graphView()->getCurrentOffset();
	m_lastOffsetB = m_paneB->graphView()->getCurrentOffset();
}

void DiffController::mirrorBlock(const ExtractedFunction& fromFunc, uint64_t offset,
    const std::map<size_t, size_t>& map, const ExtractedFunction& toFunc, DiffViewContainer* toPane)
{
	auto block = blockAtOffset(fromFunc, offset);
	if (!block)
		return;
	auto it = map.find(*block);
	if (it == map.end())
		return;
	if (it->second >= toFunc.blocks.size())
		return;
	const ExtractedBlock& target = toFunc.blocks[it->second];
	uint64_t current = toPane->graphView()->getCurrentOffset();
	// Already inside the counterpart block? Leave the pane alone.
	for (uint64_t addr : target.lineAddrs)
		if (addr == current)
			return;
	if (target.basicBlock && current >= target.basicBlock->GetStart() && current < target.basicBlock->GetEnd())
		return;
	m_syncing = true;
	toPane->graphView()->showAddress(target.firstAddr, true, true);
	m_syncing = false;
}

bool DiffController::driveSyncFrom(DiffViewContainer* driver)
{
	if (!m_panes.count(m_paneA) || !m_panes.count(m_paneB))
		return false;
	if (driver != m_paneA && driver != m_paneB)
		return false;

	const bool driverIsA = (driver == m_paneA);
	if (!m_funcA || !m_funcB)
		return false;
	const ExtractedFunction& fromFunc = driverIsA ? *m_funcA : *m_funcB;
	const ExtractedFunction& toFunc = driverIsA ? *m_funcB : *m_funcA;
	const std::map<size_t, size_t>& map = driverIsA ? m_aToB : m_bToA;
	DiffViewContainer* partner = driverIsA ? m_paneB : m_paneA;

	// Function-level coupling: the driver moved to another function. Recompute the
	// side-by-side pair from the driver, which auto-pairs the partner's counterpart.
	FunctionRef driverFunc = driver->graphView()->getCurrentFunction();
	uint64_t driverStart = driverFunc ? driverFunc->GetStart() : 0;
	uint64_t activeStart = fromFunc.function ? fromFunc.function->GetStart() : 0;
	if (driverStart && driverStart != activeStart)
	{
		m_syncing = true;
		driver->setStatus(compute(driver));
		m_syncing = false;
		return true;
	}

	// Within-function: mirror the driver's current block onto its counterpart.
	uint64_t offset = driver->graphView()->getCurrentOffset();
	mirrorBlock(fromFunc, offset, map, toFunc, partner);
	m_lastOffsetA = m_paneA->graphView()->getCurrentOffset();
	m_lastOffsetB = m_paneB->graphView()->getCurrentOffset();
	return false;
}

void DiffController::onAddressChange(ViewFrame* frame)
{
	DiffViewContainer* pane = paneForFrame(frame);
	if (!m_active || m_syncing)
		return;
	if (!pane || !paneIsCurrent(pane))
		return;
	m_lastDriver = pane;
	driveSyncFrom(pane);
}
