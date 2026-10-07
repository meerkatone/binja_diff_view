// DiffController: owns the active diffed pane pair, talks to the engine,
// applies the overlay to both functions, and keeps navigation aligned.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "diffengine.h"
#include "diffmodel.h"

// Token emphasis for one rendered IL line of a changed block: which tokens
// (by text, in order) the engine flagged as changed. Consumed by the render
// layer to mark just those tokens inline.
struct LineEmphasis
{
	uint64_t lineAddress = 0;
	size_t instructionIndex = 0;
	std::vector<bn_graph_diff::TokenSpan> spans;
};

class DiffViewContainer;
class View;
class ViewFrame;

// Overlay palette (default or colorblind-safe, per setting).
struct DiffPalette
{
	BNHighlightColor removed, added, changed, cosmetic;
};
const DiffPalette& diffPalette();

// Human-readable label for an IL form (e.g. "MLIL", "HLIL", "Disassembly").
QString ilDisplayName(const BinaryNinja::FunctionViewType& type);

class DiffController : public QObject, public BinaryNinja::BinaryDataNotification
{
	Q_OBJECT

  public:
	static DiffController& instance();

	void registerPane(DiffViewContainer* pane);
	void unregisterPane(DiffViewContainer* pane);
	void pairFrames(ViewFrame* left, ViewFrame* right);
	// Pause graph-only navigation and deferred overlay work while a workspace is
	// displaying native Linear views. Whole-binary matching may continue because
	// its result also feeds the persistent function table.
	void setGraphPresentation(ViewFrame* left, ViewFrame* right, bool active);
	// Workspace-facing helpers. They keep result navigation and graph overlay
	// activation behind the same controller used by toolbar-driven diffs.
	QString computeFrame(ViewFrame* frame);
	QString prepareFunctionMatches(ViewFrame* frame);
	// Refresh durable pair artifacts after Binary Ninja saves a participating
	// BNDB under a new modification-time identity.
	void persistMatchesForFile(BinaryNinja::FileMetadata* file);
	QString activateMatch(ViewFrame* left, ViewFrame* right, uint64_t addressA, uint64_t addressB);
	QString activateLinearMatch(ViewFrame* left, ViewFrame* right,
	    uint64_t addressA, uint64_t addressB);
	QString navigateUnmatched(ViewFrame* frame, uint64_t address);
	bn_graph_diff::FunctionMatchMapsRef matchesForFrames(
	    ViewFrame* left, ViewFrame* right) const;
	// Resolve compact leaf labels from the live BinaryView without changing the
	// qualified names stored in matching results.
	std::map<uint64_t, std::string> displayNamesForFrame(ViewFrame* frame) const;
	// Read-only planning followed by one background, undoable mutation. The
	// selected-address set is always keyed by side A, matching navigator rows.
	SymbolPortPlan planSymbolPort(ViewFrame* left, ViewFrame* right,
	    const std::set<uint64_t>& selectedA, const SymbolPortOptions& options) const;
	// Plan a manually chosen pair by its table addresses. Unlike the current-pane
	// helper, this is immune to stale cursor/current-function state in the UI.
	SymbolPortPlan planAddressPairSymbolPort(ViewFrame* left, ViewFrame* right,
	    uint64_t addressA, uint64_t addressB, const SymbolPortOptions& options) const;
	// Explicitly pair the functions currently displayed in the two graph panes.
	// This is the recovery path when automatic whole-binary matching left the
	// desired functions in the A-only/B-only sets.
	SymbolPortPlan planCurrentSymbolPort(ViewFrame* left, ViewFrame* right,
	    const SymbolPortOptions& options) const;
	bool applySymbolPort(ViewFrame* left, ViewFrame* right,
	    const SymbolPortPlan& plan, const SymbolPortOptions& options);
	bool symbolPortRunning() const { return m_symbolPortRunning; }

	// Compute (or recompute) the diff between `src` and its partner pane.
	// Returns a human-readable status for the pane's status label.
	QString compute(DiffViewContainer* src);
	QString recompute(DiffViewContainer* src);

	// Clear the overlay and the active pair.
	void clear();

	// Jump both panes to the next/previous changed block. `src` (used for the
	// status label) may be null, e.g. from a global keyboard action.
	void jumpToChange(DiffViewContainer* src, bool forward);

	bool hasActivePair() const { return m_active; }

	// Notification entry point: one pane's IL form changed. Mirrors the new IL
	// onto the partner pane, refreshes both panes' IL indicators, and recomputes
	// the overlay at the new level if a diff is active.
	void onILViewTypeChange(ViewFrame* frame, View* changedView,
	    const BinaryNinja::FunctionViewType& type);

	// Notification entry point: a pane navigated (e.g. via the `g` go-to). Drives
	// the synced counterpart navigation immediately rather than waiting for the
	// poll timer: recomputes if the pane moved to a new function, otherwise mirrors
	// the pane's current block onto its matched counterpart in the partner.
	void onAddressChange(ViewFrame* frame);

	// Thread-safe lookups for the render layer.
	std::optional<uint64_t> overlayBlockStart(BinaryNinja::BinaryView* view,
	    uint64_t funcStart, const std::vector<BinaryNinja::DisassemblyTextLine>& lines);
	std::optional<BNHighlightColor> colorForBlock(BinaryNinja::BinaryView* view,
	    uint64_t funcStart, uint64_t firstLineAddr);
	std::optional<BNHighlightColor> colorForLine(BinaryNinja::BinaryView* view,
	    uint64_t funcStart, uint64_t firstLineAddr, uint64_t lineAddr, size_t instructionIndex);
	std::optional<LineEmphasis> emphasisForLine(BinaryNinja::BinaryView* view,
	    uint64_t funcStart, uint64_t firstLineAddr, uint64_t lineAddr, size_t instructionIndex);
	std::optional<BNHighlightColor> colorForLinearLine(BinaryNinja::BinaryView* view,
	    uint64_t funcStart, uint64_t lineAddr, size_t instructionIndex);
	std::optional<LineEmphasis> emphasisForLinearLine(BinaryNinja::BinaryView* view,
	    uint64_t funcStart, uint64_t lineAddr, size_t instructionIndex);

	void OnAnalysisFunctionAdded(BinaryNinja::BinaryView* view, BinaryNinja::Function*) override;
	void OnAnalysisFunctionRemoved(BinaryNinja::BinaryView* view, BinaryNinja::Function*) override;
	void OnAnalysisFunctionUpdated(BinaryNinja::BinaryView* view, BinaryNinja::Function*) override;
	void OnSymbolAdded(BinaryNinja::BinaryView* view, BinaryNinja::Symbol*) override;
	void OnSymbolRemoved(BinaryNinja::BinaryView* view, BinaryNinja::Symbol*) override;
	void OnSymbolUpdated(BinaryNinja::BinaryView* view, BinaryNinja::Symbol*) override;
	void OnBinaryDataWritten(BinaryNinja::BinaryView* view, uint64_t, size_t) override;
	void OnBinaryDataInserted(BinaryNinja::BinaryView* view, uint64_t, size_t) override;
	void OnBinaryDataRemoved(BinaryNinja::BinaryView* view, uint64_t, uint64_t) override;

	signals:
	void functionMatchingStarted(ViewFrame* left, ViewFrame* right);
	void functionMatchesUpdated(ViewFrame* left, ViewFrame* right);
	void functionMatchingFailed(ViewFrame* left, ViewFrame* right, const QString& error);
	void matchActivationFinished(ViewFrame* left, ViewFrame* right, const QString& status);
	void ilViewTypeUpdated(ViewFrame* frame, const QString& displayName);
	void symbolPortFinished(ViewFrame* left, ViewFrame* right, bool success,
	    const QString& message);

  private:
	using PendingMatchKey = std::pair<uintptr_t, uintptr_t>;
	explicit DiffController(QObject* parent = nullptr);

	DiffViewContainer* findPartner(DiffViewContainer* src) const;
	bool isCandidatePane(DiffViewContainer* src, DiffViewContainer* partner) const;
	DiffViewContainer* paneForFrame(ViewFrame* frame) const;
	void applyOverlay(const bn_graph_diff::BlockDiffResult& result);
	void highlightBlock(const bn_graph_diff::ExtractedFunction& f,
	    const bn_graph_diff::ExtractedBlock& block, const BNHighlightColor& color);
	void highlightLine(const bn_graph_diff::ExtractedFunction& f,
	    const bn_graph_diff::ExtractedBlock& block, uint64_t addr, size_t instructionIndex,
	    const BNHighlightColor& color);
	std::optional<size_t> blockAtOffset(
	    const bn_graph_diff::ExtractedFunction& f, uint64_t offset) const;

	// Mirror the block at `offset` in `fromFunc` onto its counterpart in `toFunc`,
	// navigating `toPane` there. No-op if the pane is already inside the counterpart.
	void mirrorBlock(const bn_graph_diff::ExtractedFunction& fromFunc, uint64_t offset,
	    const std::map<size_t, size_t>& map, const bn_graph_diff::ExtractedFunction& toFunc,
	    DiffViewContainer* toPane);

	// Drive synced counterpart navigation from `driver`. Returns true if it
	// recomputed for a new function (caller should stop further sync work).
	bool driveSyncFrom(DiffViewContainer* driver);
	void beginFunctionMatch(DiffViewContainer* src, DiffViewContainer* partner);
	QString computeImpl(DiffViewContainer* src, bool allowHiddenGraph);
	QString renderCurrentPair(DiffViewContainer* src, DiffViewContainer* partner,
	    bn_graph_diff::FunctionMatchMapsRef matchMaps, bool allowHiddenGraph);
	void retryFunctionMatchWhenAnalysisSettles(const PendingMatchKey& key, int stableIdleChecks);
	void retrySelectedDiffWhenAnalysisSettles(QPointer<DiffViewContainer> source,
	    QPointer<DiffViewContainer> partner, uint64_t generation, int stableIdleChecks,
	    bool allowHiddenGraph);
	void finishMatchActivation(QPointer<DiffViewContainer> paneA,
	    QPointer<DiffViewContainer> paneB, uint64_t addressA, uint64_t addressB,
	    uint64_t generation, int attemptsRemaining, bool allowHiddenGraph = false);
	void finishLinearMatchWhenReady(QPointer<DiffViewContainer> paneA,
	    QPointer<DiffViewContainer> paneB, uint64_t addressA, uint64_t addressB,
	    uint64_t generation, int attemptsRemaining);
	void invalidateView(BinaryNinja::BinaryView* view);
	void invalidateSymbolView(BinaryNinja::BinaryView* view);
	void invalidateOverlayCache(BinaryNinja::BinaryView* view);

	struct OverlayCacheKey
	{
		uintptr_t viewA = 0, viewB = 0;
		uint64_t functionA = 0, functionB = 0;
		int viewType = 0;
		std::string viewTypeName;
		const bn_graph_diff::FunctionMatchMaps* matchMaps = nullptr;
		bool operator==(const OverlayCacheKey& other) const
		{
			return viewA == other.viewA && viewB == other.viewB
			    && functionA == other.functionA && functionB == other.functionB
			    && viewType == other.viewType && viewTypeName == other.viewTypeName
			    && matchMaps == other.matchMaps;
		}
	};
	struct OverlayCacheEntry
	{
		OverlayCacheKey key;
		bn_graph_diff::FunctionMatchMapsRef matchMaps;
		std::shared_ptr<const bn_graph_diff::ExtractedFunction> functionA, functionB;
		std::shared_ptr<const bn_graph_diff::BlockDiffResult> result;
	};
	std::optional<OverlayCacheEntry> cachedOverlay(const OverlayCacheKey& key);
	bool cacheOverlay(OverlayCacheEntry entry, uint64_t revision);

	void syncTimerTick();

	std::set<DiffViewContainer*> m_panes;
	struct FramePartner
	{
		ViewFrame* frame;
		bool baseline;
	};
	std::map<ViewFrame*, FramePartner> m_explicitPartners;
	std::map<BNBinaryView*, std::pair<bn_graph_diff::BinaryViewRef, size_t>> m_observedViews;
	struct PendingMatchWaiter
	{
		QPointer<DiffViewContainer> source;
		QPointer<DiffViewContainer> partner;
	};
	std::map<PendingMatchKey, std::vector<PendingMatchWaiter>> m_pendingMatches;
	bool m_symbolPortRunning = false;
	std::atomic<BNBinaryView*> m_symbolPortTarget {nullptr};
	uint64_t m_activationGeneration = 0;
	uint64_t m_selectedDiffRetryGeneration = 0;
	std::atomic<uint64_t> m_overlayRevision {0};
	std::shared_ptr<std::atomic<bool>> m_selectedDiffCancelled;

	bool m_active = false;
	DiffViewContainer* m_paneA = nullptr;
	DiffViewContainer* m_paneB = nullptr;
	DiffViewContainer* m_lastDriver = nullptr;
	std::shared_ptr<const bn_graph_diff::ExtractedFunction> m_funcA, m_funcB;
	bn_graph_diff::FunctionMatchMapsRef m_matchMaps;
	// counterpart block index per side (block index -> partner block index)
	std::map<size_t, size_t> m_aToB, m_bToA;
	// Ordered "change sites": (addr in A or 0, addr in B or 0, valid flags).
	struct ChangeSite
	{
		std::optional<uint64_t> addrA, addrB;
	};
	std::vector<ChangeSite> m_changes;
	std::optional<size_t> m_changeCursor;

	// View-local render state. Including BinaryView identity prevents the usual
	// same-image-base patch pair from sharing one side's token/color state.
	std::mutex m_overlayMutex;
	std::map<OverlayBlockKey, BNHighlightColor> m_blockColors;
	std::map<OverlayLineKey, BNHighlightColor> m_lineColors;
	std::map<OverlayLineKey, LineEmphasis> m_lineEmphasis;
	std::map<OverlayLinearLineKey, LineEmphasis> m_linearEmphasis;
	std::map<OverlayLinearLineKey, BNHighlightColor> m_linearLineColors;
	// HLIL body formatting may change a line address while retaining its stable
	// instruction index. Keep a direct fallback index rather than scanning every
	// overlay entry for every rendered line. The stored address preserves the old
	// map-order rule when more than one line shares an instruction index.
	using OverlayInstructionKey = std::tuple<uintptr_t, uint64_t, size_t>;
	std::map<OverlayInstructionKey, std::pair<uint64_t, LineEmphasis>> m_linearEmphasisByInstruction;
	std::map<OverlayInstructionKey, std::pair<uint64_t, BNHighlightColor>> m_linearColorsByInstruction;
	// Recent selected-function diffs contain only immutable extraction and engine
	// output. Reusing them avoids repeating Binary Ninja graph extraction and FFI
	// work while moving back and forth through the results table. Every analysis,
	// symbol, or byte notification for either view invalidates affected entries.
	std::mutex m_overlayCacheMutex;
	std::list<OverlayCacheEntry> m_overlayCache;
	static constexpr size_t OverlayCacheLimit = 8;

	QTimer* m_syncTimer;
	bool m_syncing = false;
	// Guards against re-entrant IL notifications while we mirror IL across panes.
	bool m_ilSyncing = false;
	uint64_t m_lastOffsetA = ~0ull, m_lastOffsetB = ~0ull;
	BinaryNinja::FunctionViewType m_lastTypeA, m_lastTypeB;
};
