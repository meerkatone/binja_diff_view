// Bridge to the platform rust_diff shared library plus function/block
// extraction from Binary Ninja into the engine's JSON contracts.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "binaryninjaapi.h"

class QSaveFile;
#include "weightedcache.h"
#include "sharedresultcache.h"
#include "keyedworkgate.h"

namespace bn_graph_diff {

using FunctionRef = BinaryNinja::Ref<BinaryNinja::Function>;
using BasicBlockRef = BinaryNinja::Ref<BinaryNinja::BasicBlock>;
using BinaryViewRef = BinaryNinja::Ref<BinaryNinja::BinaryView>;

// Return the stable symbol name used as matching evidence. Preserve a valid
// qualified short name so methods from different classes do not collapse onto
// the same leaf identifier. Some PE/PDB symbols leave the short spelling empty
// or decorated; repair those from the raw ABI symbol.
std::string functionMatchName(BinaryViewRef view,
	BinaryNinja::Ref<BinaryNinja::Symbol> symbol);

// Return the unqualified leaf identifier shown in result rows and pair status.
// This is deliberately separate from functionMatchName so compact UI labels do
// not weaken name-based matching.
std::string functionDisplayName(BinaryViewRef view,
	BinaryNinja::Ref<BinaryNinja::Symbol> symbol);

// Decorative/volatile token types excluded from the diff (comments, tags…).
// Shared with the render layer so line identification matches the extraction.
// True while the calling thread is requesting a function's IL for whole-binary
// extraction. Binary Ninja reports the resulting function update on that thread.
bool materializingFunctionAnalysis();

inline bool tokenIgnoredForDiff(BNInstructionTextTokenType type)
{
	switch (type)
	{
	case CommentToken:
	case AnnotationToken:
	case TagToken:
	case CollapseStateIndicatorToken:
		return true;
	default:
		return false;
	}
}

// One extracted basic block at the chosen IL level, in display order.
struct ExtractedBlock
{
	size_t index = 0;             // position in the flow graph's node list
	uint64_t firstAddr = 0;       // address of the first instruction line
	std::vector<uint64_t> lineAddrs;  // address per IL line (parallel to lines)
	std::vector<size_t> lineInstrIndices;  // BN instruction index per IL line
	std::vector<size_t> successors;   // indices into the extraction
	BasicBlockRef basicBlock;     // IL (or disasm) block backing the node
	std::string linesJson;        // pre-serialized [{tokens,text},...] for the engine
};

struct ExtractedFunction
{
	FunctionRef function;
	BinaryNinja::FunctionViewType viewType;
	std::vector<ExtractedBlock> blocks;
	size_t entry = 0;
};

enum class BlockStatus
{
	Equal,
	Cosmetic,
	Changed
};

struct BlockPair
{
	size_t a, b;
	BlockStatus status;
	double similarity;
};

// Per-line ops of the engine's IL diff, reduced to what the overlay needs.
enum class LineOp
{
	Equal,
	Rename,
	Replace,
	Insert,
	Delete
};

struct TokenSpan
{
	std::string text;
	bool changed;
};

struct DiffLine
{
	LineOp op;
	// Original rendered text is required for whole-line insert/delete records,
	// whose token-span arrays are intentionally empty.
	std::string aText, bText;
	// Per-token spans of the A/B side for replace/rename lines (token-level
	// LCS from the engine); empty for whole-line ops.
	std::vector<TokenSpan> aSpans, bSpans;
};

struct LineDiff
{
	std::vector<DiffLine> lines;
};

struct BlockDiffResult
{
	std::vector<BlockPair> pairs;
	std::vector<size_t> onlyA, onlyB;
	std::map<std::pair<size_t, size_t>, LineDiff> lineDiffs;
	std::optional<double> ilSimilarity;
	double similarity = 0.0;
};

// Full function-level records returned by rust_diff_diff_json. Keeping these
// in the native frontend lets the combined workspace expose the useful
// whole-binary triage surface from rust_diff without loading its second,
// independent Python UI.
struct FunctionSummary
{
	std::string name;
	uint64_t address = 0;
	uint64_t size = 0;
	size_t instructionCount = 0;
	uint64_t suggestedCounterpartAddress = 0;
	std::string suggestedCounterpartName;
	double suggestedBodySimilarity = 0.0;
	std::string securityState = "Unknown";
	double securityScore = 0.0;
	double securityDirectScore = 0.0;
	std::vector<std::string> securityEvidence;
	std::vector<std::string> securityCounterEvidence;
	bool securityPropagated = false;
};

struct FunctionMatchRecord
{
	FunctionSummary a, b;
	double similarity = 0.0;
	double confidence = 0.0;
	double ambiguityMargin = 1.0;
	std::string matchType;
	std::string contentMatch = "Changed";
	double cfgSimilarity = 0.0;
	double basicBlockSimilarity = 0.0;
	double instructionSimilarity = 0.0;
	double edgeSimilarity = 0.0;
	double nameSimilarity = 0.0;
	double callSimilarity = 0.0;
	size_t neighborSupport = 0, neighborContradictions = 0;
	bool bindingAnchor = false;
	std::string securityState = "Unknown";
	double securityScore = 0.0;
	double securityDirectScore = 0.0;
	std::vector<std::string> securityEvidence;
	std::vector<std::string> securityCounterEvidence;
	bool securityPropagated = false;
};

// Callee rename/address maps and navigator records derived from a whole-binary
// function match run. Resolving the maps in the IL canonicalizer makes a
// renamed-but-matched callee cosmetic instead of a false change.
struct FunctionMatchMaps
{
	std::map<std::string, std::string> renameMap;  // symbol-in-A -> symbol-in-B
	std::map<std::string, std::string> addrMap;    // "0x..."-in-A -> "0x..."-in-B
	std::map<uint64_t, uint64_t> funcMap;          // function start A -> B (all matches)
	std::vector<FunctionMatchRecord> matches;
	std::vector<FunctionSummary> unmatchedA, unmatchedB;
	size_t matchCount = 0;
	double similarity = 0.0;
	double matchedSimilarity = 0.0;
	double coverage = 0.0;
	double analysisTime = 0.0;
	// True only for a completed result restored from the durable pair artifact.
	// This lets workspace startup render the saved snapshot immediately instead
	// of waiting for unrelated lazy analysis started by a newly-created graph.
	bool restoredFromDisk = false;
	bool valid = true;
	std::string error;
};

// A text-only callee token cannot identify which of several same-named
// functions it references. Only use name normalization when the name is
// unique on both sides; address tokens use addrMap separately.
inline void rebuildCalleeRenameMap(FunctionMatchMaps& maps)
{
	std::map<std::string, size_t> countA, countB;
	auto count = [](auto& names, const FunctionSummary& function) {
		if (!function.name.empty()) names[function.name]++;
	};
	for (const auto& match : maps.matches)
	{
		count(countA, match.a);
		count(countB, match.b);
	}
	for (const auto& function : maps.unmatchedA) count(countA, function);
	for (const auto& function : maps.unmatchedB) count(countB, function);
	maps.renameMap.clear();
	for (const auto& match : maps.matches)
		if (!match.a.name.empty() && match.a.name != match.b.name
		    && countA[match.a.name] == 1 && countB[match.b.name] == 1)
			maps.renameMap[match.a.name] = match.b.name;
}

using FunctionMatchMapsRef = std::shared_ptr<const FunctionMatchMaps>;

// Persistent artifacts and patch signatures use these values to reject data
// produced by an incompatible extraction or matching implementation.
inline constexpr int CurrentMatchSchemaVersion = 25;
inline constexpr int CurrentExtractionSchemaVersion = 12;

// A prepared result is valid only while the exact current object and both view
// epochs remain current. UI construction may span event turns, so carrying the
// cache identity prevents an invalidated result from being restored later.
struct PreparedFunctionMatches
{
	FunctionMatchMapsRef matches;
	size_t sessionIdA = 0;
	std::string typeNameA;
	size_t sessionIdB = 0;
	std::string typeNameB;
	uint64_t epochA = 0;
	uint64_t epochB = 0;
};

class DiffEngine
{
  public:
	// Loads the platform rust_diff shared library on first use.
	static DiffEngine& instance();

	bool available() const { return m_blockDiff != nullptr; }
	const std::string& loadError() const { return m_loadError; }

	// Extract a function's CFG at the given IL level as the engine's block
	// representation. Returns std::nullopt when the graph has no nodes yet.
	std::optional<ExtractedFunction> extract(
	    FunctionRef func, const BinaryNinja::FunctionViewType& viewType);

	std::optional<BlockDiffResult> blockDiff(const ExtractedFunction& a,
	    const ExtractedFunction& b, const FunctionMatchMaps& maps);

	// Line-level diff of one block pair (drives instruction highlights).
	std::optional<LineDiff> lineDiff(const ExtractedBlock& a, const ExtractedBlock& b,
	    const FunctionMatchMaps& maps);

	// Whole-function semantic IL-line similarity (0..1): runs the IL diff over the
	// two functions' full token streams in block order, mirroring the rust_diff
	// text view's "semantic similarity" score. Returns nullopt if unavailable.
	std::optional<double> ilSimilarity(const ExtractedFunction& a, const ExtractedFunction& b,
	    const FunctionMatchMaps& maps);

	// Whole-binary function match between two views (cached per stable file/view
	// identity). This may be expensive and is safe to call from a worker thread.
	FunctionMatchMaps matchFunctions(BinaryViewRef a, BinaryViewRef b,
	    BinaryNinja::Ref<BinaryNinja::BackgroundTask> task = nullptr);
	// Always returns a result (inspect valid/error). Retain it across worker/UI
	// handoff so an oversized result does not depend on cache ownership.
	FunctionMatchMapsRef matchFunctionsRef(BinaryViewRef a, BinaryViewRef b,
	    BinaryNinja::Ref<BinaryNinja::BackgroundTask> task = nullptr);
	std::optional<FunctionMatchMaps> cachedFunctionMatches(BinaryViewRef a, BinaryViewRef b) const;
	// Immutable cache view for read-only UI consumers. This avoids copying the
	// complete function table on every refresh or selection update.
	FunctionMatchMapsRef cachedFunctionMatchesRef(BinaryViewRef a, BinaryViewRef b) const;
	// Capture a cache result together with the view epochs and exact cache object
	// that made it current. A prepared result must be revalidated after any event
	// turn before a workspace consumes it.
	std::optional<PreparedFunctionMatches> prepareCachedFunctionMatches(
	    BinaryViewRef a, BinaryViewRef b) const;
	bool preparedFunctionMatchesCurrent(BinaryViewRef a, BinaryViewRef b,
	    const PreparedFunctionMatches& prepared) const;
	// Stable, read-only identity used by extraction and pair caches. A missing
	// value means the view is dirty, still analyzing, or not backed by a saved file.
	std::optional<std::string> persistentIdentity(BinaryViewRef view) const;
	static int matchSchemaVersion();
	static int extractionSchemaVersion();
	// Replace only the exact cache result used to build a symbol-port plan. A
	// structural/data invalidation removes that object and makes this operation
	// fail instead of reviving stale address mappings.
	bool replaceFunctionMatchesIfCurrent(BinaryViewRef a, BinaryViewRef b,
	    const FunctionMatchMapsRef& expected, const FunctionMatchMaps& replacement);
	FunctionMatchMapsRef replaceFunctionMatchesIfCurrentRef(BinaryViewRef a, BinaryViewRef b,
	    const FunctionMatchMapsRef& expected, const FunctionMatchMaps& replacement);
	// Rewrite a completed in-memory result using the views' current saved-file
	// identities. Called after either BNDB is saved, because saving changes the
	// file timestamp used by the durable cache key.
	bool persistCachedFunctionMatches(BinaryViewRef a, BinaryViewRef b) const;
	bool persistCachedBinaryExtraction(BinaryViewRef view) const;
	void invalidateMatches(BinaryViewRef view);
	// Drop one view's reusable extraction without discarding a deliberately
	// republished pair match (used during controlled symbol-port operations).
	void invalidateExtraction(BinaryViewRef view);
	void invalidateMatchPair(BinaryViewRef a, BinaryViewRef b);

  private:
	DiffEngine();
	~DiffEngine();

	using JsonFn = char* (*)(const char*);
	using Json2Fn = char* (*)(const char*, const char*);
	using CancelFn = bool (*)(void*);
	using Json2CancelFn = char* (*)(const char*, const char*, CancelFn, void*);
	using PrepareFn = void* (*)(const char*, CancelFn, void*);
	using FreePreparedFn = void (*)(void*);
	using PreparedSizeFn = size_t (*)(const void*);
	using DiffPreparedFn = char* (*)(const void*, const void*, CancelFn, void*);
	using FreeFn = void (*)(char*);

	std::string call(JsonFn fn, const std::string& request);

	JsonFn m_blockDiff = nullptr;
	JsonFn m_ilDiff = nullptr;
	Json2Fn m_diffJson = nullptr;
	Json2CancelFn m_diffJsonCancellable = nullptr;
	FreeFn m_freeString = nullptr;
	PrepareFn m_prepareJson = nullptr;
	FreePreparedFn m_freePrepared = nullptr;
	PreparedSizeFn m_preparedSize = nullptr;
	DiffPreparedFn m_diffPrepared = nullptr;
	std::string m_loadError;
	class LibraryHolder;
	std::unique_ptr<LibraryHolder> m_library;

	struct ViewKey
	{
		size_t sessionId = 0;
		std::string typeName;
		bool operator<(const ViewKey& other) const
		{
			return std::tie(sessionId, typeName) < std::tie(other.sessionId, other.typeName);
		}
		bool operator==(const ViewKey& other) const
		{
			return sessionId == other.sessionId && typeName == other.typeName;
		}
	};
	using MatchKey = std::pair<ViewKey, ViewKey>;
	static ViewKey viewKey(BinaryViewRef view);
	static std::optional<std::string> persistentViewIdentity(BinaryViewRef view);
	static std::optional<std::string> persistentCachePath(BinaryViewRef a, BinaryViewRef b);
	static std::optional<std::string> persistentExtractionCachePath(BinaryViewRef view);
	struct BinaryExtraction
	{
		explicit BinaryExtraction(std::string text) : json(std::move(text)) {}
		const std::string json;
		std::timed_mutex prepareMutex;
		std::shared_ptr<void> prepared;
	};
	using BinaryExtractionRef = std::shared_ptr<BinaryExtraction>;
	std::shared_ptr<void> prepareBinaryCached(BinaryViewRef view,
	    const BinaryExtractionRef& extraction, BinaryNinja::Ref<BinaryNinja::BackgroundTask> task);
	BinaryExtractionRef loadPersistentBinaryExtraction(BinaryViewRef view) const;
	// `stillCurrent` runs with m_matchMutex held, after the file identity is
	// hashed and immediately before the artifact's atomic rename. Invalidation
	// removes artifacts under the same mutex, so a write commits only while the
	// snapshot is still valid: unchanged view epochs and, for a pair, no Rematch.
	// Validity does not depend on memory retention, which a zero or exceeded
	// cache budget can decline.
	using PersistenceCheck = std::function<bool()>;
	bool commitIfCurrent(QSaveFile& file, const PersistenceCheck& stillCurrent) const;
	void storePersistentBinaryExtraction(BinaryViewRef view, const std::string& extraction,
	    const PersistenceCheck& stillCurrent) const;
	BinaryExtractionRef extractBinaryForMatchCached(BinaryViewRef view,
	    BinaryNinja::Ref<BinaryNinja::BackgroundTask> task, const char* side);
	std::optional<FunctionMatchMaps> loadPersistentFunctionMatches(
	    BinaryViewRef a, BinaryViewRef b) const;
	void storePersistentFunctionMatches(BinaryViewRef a, BinaryViewRef b,
	    const FunctionMatchMaps& maps, const PersistenceCheck& stillCurrent) const;
	mutable std::mutex m_matchMutex;
	KeyedWorkGate<MatchKey> m_matchWork;
	KeyedWorkGate<ViewKey> m_extractionWork;
	mutable SharedResultCache<MatchKey, FunctionMatchMaps> m_matchCache;
	mutable WeightedLruCache<ViewKey, BinaryExtractionRef> m_extractionCache;
	std::map<ViewKey, uint64_t> m_viewEpoch;
	// Rematch discards one pair without an analysis change, so it advances this
	// count rather than the view epochs.
	std::map<MatchKey, uint64_t> m_pairInvalidation;
	// Require m_matchMutex. An absent entry has never been invalidated: 0.
	uint64_t viewEpoch(const ViewKey& key) const
	{
		auto found = m_viewEpoch.find(key);
		return found == m_viewEpoch.end() ? 0 : found->second;
	}
	uint64_t pairInvalidation(const MatchKey& key) const
	{
		auto found = m_pairInvalidation.find(key);
		return found == m_pairInvalidation.end() ? 0 : found->second;
	}
};

}  // namespace bn_graph_diff
