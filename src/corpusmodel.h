// Pure corpus planning and aggregation types. This header deliberately avoids
// Binary Ninja and Qt so the planning rules remain cheap to unit test.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bn_graph_diff {

using CorpusBinaryId = uint32_t;
using FunctionFamilyId = uint64_t;

enum class CorpusMode
{
	Reference,
	Lineage,
	AllPairs
};

struct CorpusBinary
{
	CorpusBinaryId id = 0;
	std::string displayName;
	std::string persistentIdentity;
	size_t functionCount = 0;
	size_t instructionCount = 0;
	bool dirty = false;
};

struct CorpusEdgeKey
{
	CorpusBinaryId source = 0;
	CorpusBinaryId target = 0;

	bool operator==(const CorpusEdgeKey&) const = default;
	bool operator<(const CorpusEdgeKey& other) const
	{
		return source < other.source || (source == other.source && target < other.target);
	}
};

struct CorpusPlan
{
	CorpusMode mode = CorpusMode::Reference;
	std::vector<CorpusBinaryId> binaryOrder;
	std::optional<CorpusBinaryId> reference;
	std::vector<CorpusEdgeKey> edges;
	bool referenceOverlay = false;
	bool valid = false;
	std::string error;
};

// Build a deterministic directed plan. All-pairs emits one orientation for
// each unordered pair, following the supplied binary order.
CorpusPlan buildCorpusPlan(CorpusMode mode,
	const std::vector<CorpusBinaryId>& binaryOrder,
	std::optional<CorpusBinaryId> reference = std::nullopt,
	bool referenceOverlay = false, size_t maximumPairs = 45);

// Natural filename ordering for release corpora. Numeric runs compare by value,
// common binary/database suffixes are ignored, and known prerelease suffixes
// sort before the corresponding final release.
bool naturalReleaseNameLess(const std::string& left, const std::string& right);
std::vector<size_t> naturalReleaseOrder(const std::vector<std::string>& names);

enum class CorpusPairState
{
	Queued,
	Running,
	Cached,
	Complete,
	Cancelled,
	Failed,
	Invalidated
};

struct CorpusCompletionSummary
{
	size_t total = 0;
	size_t successful = 0;
	size_t failed = 0;
	size_t cancelled = 0;
	size_t pending = 0;
	bool partial = false;
};

CorpusCompletionSummary summarizeCorpusCompletion(
	const std::vector<CorpusPairState>& states);

struct CorpusMatchSample
{
	size_t sourceInstructionCount = 0;
	size_t targetInstructionCount = 0;
	double similarity = 0.0;
	double confidence = 0.0;
	std::string matchType;
	std::string contentMatch;
};

struct CorpusPairMetrics
{
	size_t sourceFunctionCount = 0;
	size_t targetFunctionCount = 0;
	size_t sourceInstructionCount = 0;
	size_t targetInstructionCount = 0;
	size_t acceptedMatchCount = 0;
	size_t exactMatchCount = 0;
	size_t changedMatchCount = 0;
	size_t sourceOnlyFunctionCount = 0;
	size_t targetOnlyFunctionCount = 0;
	size_t sourceOnlyInstructionCount = 0;
	size_t targetOnlyInstructionCount = 0;
	double sourceCoverage = 0.0;
	double targetCoverage = 0.0;
	double instructionCoverageSource = 0.0;
	double instructionCoverageTarget = 0.0;
	double meanSimilarity = 0.0;
	double medianSimilarity = 0.0;
	double meanConfidence = 0.0;
	std::map<std::string, size_t> matchesByMethod;
};

CorpusPairMetrics calculateCorpusMetrics(size_t sourceFunctionCount,
	size_t targetFunctionCount, size_t sourceInstructionCount,
	size_t targetInstructionCount, const std::vector<CorpusMatchSample>& matches,
	size_t sourceOnlyInstructionCount = 0,
	size_t targetOnlyInstructionCount = 0);

CorpusPairMetrics invertCorpusMetrics(const CorpusPairMetrics& metrics);

// Publish full snapshot totals from a successful pair's raw metrics. A binary
// may participate in several pairs; its totals are replaced, never accumulated.
void updateCorpusBinaryCounts(std::vector<CorpusBinary>& binaries,
	const CorpusEdgeKey& edge, const CorpusPairMetrics& rawMetrics);

struct FunctionOccurrence
{
	CorpusBinaryId binary = 0;
	uint64_t address = 0;
	std::string name;
	size_t instructionCount = 0;

	bool operator==(const FunctionOccurrence&) const = default;
};

struct FunctionOccurrenceKey
{
	CorpusBinaryId binary = 0;
	uint64_t address = 0;

	bool operator==(const FunctionOccurrenceKey&) const = default;
	bool operator<(const FunctionOccurrenceKey& other) const
	{
		return binary < other.binary || (binary == other.binary && address < other.address);
	}
};

struct FamilyLinkEvidence
{
	FunctionOccurrence source;
	FunctionOccurrence target;
	double similarity = 0.0;
	double confidence = 0.0;
	std::string matchType;
	CorpusEdgeKey origin;
	std::string contentMatch;

	bool operator==(const FamilyLinkEvidence&) const = default;
};

struct CorpusPairEvidence
{
	CorpusEdgeKey edge;
	std::vector<FamilyLinkEvidence> links;
	std::vector<FunctionOccurrence> unmatchedSource;
	std::vector<FunctionOccurrence> unmatchedTarget;
};

enum class FunctionFamilyState
{
	Identical,
	Changed,
	Missing,
	Unique,
	Ambiguous,
	Conflicting
};

struct FunctionFamily
{
	FunctionFamilyId id = 0;
	std::map<CorpusBinaryId, FunctionOccurrence> members;
	std::vector<FamilyLinkEvidence> acceptedLinks;
	std::vector<FamilyLinkEvidence> rejectedLinks;
	FunctionFamilyState state = FunctionFamilyState::Unique;
	bool ambiguous = false;
	bool conflicting = false;
	double minimumSimilarity = 0.0;
	double minimumConfidence = 0.0;
};

struct FamilyBuildSettings
{
	double minimumSimilarity = 0.0;
	double minimumConfidence = 0.0;
	double conflictThreshold = 0.8;
	bool requireMutualBestHeuristics = true;
};

// Reference construction never uses transitive target-to-target links.
std::vector<FunctionFamily> buildReferenceFamilies(CorpusBinaryId reference,
	const std::vector<CorpusBinaryId>& binaryOrder,
	const std::vector<CorpusPairEvidence>& pairs,
	const FamilyBuildSettings& settings = {});

// Lineage and all-pairs construction use deterministic constrained merging.
// A merge that would add a second occurrence from one binary is rejected.
std::vector<FunctionFamily> buildConstrainedFamilies(CorpusMode mode,
	const std::vector<CorpusBinaryId>& binaryOrder,
	const std::vector<CorpusPairEvidence>& pairs,
	const FamilyBuildSettings& settings = {});

const char* functionFamilyStateName(FunctionFamilyState state);

enum class PatchState
{
	VulnerableLike,
	FixedLike,
	IndependentlyModified,
	Absent,
	Ambiguous,
	NotAnalyzed
};

struct PatchClassification
{
	CorpusBinaryId binary = 0;
	FunctionOccurrence occurrence;
	PatchState state = PatchState::NotAnalyzed;
	double vulnerableSimilarity = 0.0;
	double fixedSimilarity = 0.0;
	double scoreMargin = 0.0;
	double patchRegionCoverage = 0.0;
	std::vector<std::string> evidence;
};

// One anchor patch block observed while comparing it with a target. A paired
// block contributes its measured similarity. A confirmed unmatched block is
// still evaluated, with similarity zero. Only extraction or mapping failures
// leave an observation unevaluated.
struct PatchRegionObservation
{
	bool evaluated = false;
	double similarity = 0.0;
};

struct PatchRegionScore
{
	double similarity = 0.0;
	double coverage = 0.0;
};

PatchRegionScore calculatePatchRegionScore(
	const std::vector<PatchRegionObservation>& observations);

// A signature may consist entirely of blocks added on the fixed side or
// removed from the vulnerable side. In that case the missing anchor region is
// represented by the complement of the observed structural feature score.
PatchRegionScore complementaryPatchRegionScore(const PatchRegionScore& observed);

enum class FunctionLineageState
{
	Absent,
	Present,
	Introduced,
	Reappeared,
	Identical,
	Changed,
	VulnerableLike,
	FixedLike,
	IndependentlyModified,
	Ambiguous,
	NotAnalyzed
};

struct FunctionLineageCell
{
	FunctionLineageState state = FunctionLineageState::Absent;
	std::optional<FunctionOccurrence> occurrence;
	std::optional<double> similarity;
	std::optional<double> confidence;
	std::string matchType;
};

// Describe one function-family occurrence in release order. Patch evidence,
// when present, takes priority over the general structural lineage state.
FunctionLineageCell classifyFunctionLineage(const FunctionFamily& family,
	const std::vector<CorpusBinaryId>& binaryOrder, size_t position,
	const PatchClassification* patch = nullptr);

const char* functionLineageStateName(FunctionLineageState state);

enum class FunctionTransitionState
{
	NoOccurrence,
	Unchanged,
	Modified,
	Introduced,
	Removed,
	Reappeared,
	Unresolved,
	PatchBoundary,
	ClassificationChanged
};

struct FunctionTransitionCell
{
	FunctionTransitionState state = FunctionTransitionState::NoOccurrence;
	std::optional<FunctionOccurrence> source;
	std::optional<FunctionOccurrence> target;
	std::optional<double> similarity;
	std::optional<double> confidence;
	std::string matchType;
};

// Describe the change between two adjacent releases. This is deliberately a
// transition rather than another per-build state so the UI can show the exact
// boundary where code appeared, disappeared, or changed.
FunctionTransitionCell classifyFunctionTransition(const FunctionFamily& family,
	const std::vector<CorpusBinaryId>& binaryOrder, size_t transitionPosition,
	const PatchClassification* sourcePatch = nullptr,
	const PatchClassification* targetPatch = nullptr);

const char* functionTransitionStateName(FunctionTransitionState state);

PatchState classifyPatchEvidence(double vulnerableSimilarity,
	double fixedSimilarity, double vulnerableCoverage, double fixedCoverage,
	double minimumScoreMargin = 0.1, double minimumPatchCoverage = 0.6,
	double independentModificationCeiling = 0.7);

const char* patchStateName(PatchState state);

}  // namespace bn_graph_diff
