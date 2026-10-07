#include "corpusmodel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

using namespace bn_graph_diff;

static void checkAt(bool condition, const char* expression, int line)
{
	if (!condition)
	{
		std::fprintf(stderr, "check failed at line %d: %s\n", line, expression);
		std::abort();
	}
}

#define check(condition) checkAt((condition), #condition, __LINE__)

static bool close(double left, double right)
{
	return std::abs(left - right) < 0.000001;
}

static void testNaturalReleaseOrdering()
{
	std::vector<std::string> names {"ffmpeg-6.0.bndb", "ffmpeg-7.0.bndb",
	    "ffmpeg-7.0.1.bndb", "ffmpeg-7.0.2.bndb", "ffmpeg-7.1.bndb",
	    "ffmpeg-7.1.1.bndb", "ffmpeg-6.1.2.bndb", "ffmpeg-6.1.1.bndb",
	    "ffmpeg-5.1.6.bndb", "ffmpeg-6.0.1.bndb"};
	check(naturalReleaseOrder(names) == std::vector<size_t>(
	    {8, 0, 9, 7, 6, 1, 2, 3, 4, 5}));
	check(naturalReleaseNameLess("tool-7.0-rc2.bndb", "tool-7.0.bndb"));
	check(naturalReleaseNameLess("tool-7.0-rc2.bndb", "tool-7.0-rc10.bndb"));
	check(naturalReleaseNameLess("tool-7.0.bndb", "tool-7.0.1.bndb"));
	check(naturalReleaseNameLess("TOOL-9.bndb", "tool-10.bndb"));
	std::vector<std::string> unversioned {"zeta.bndb", "alpha.bndb", "alpha.bndb"};
	check(naturalReleaseOrder(unversioned) == std::vector<size_t>({1, 2, 0}));
}

static void testReferencePlan()
{
	auto plan = buildCorpusPlan(CorpusMode::Reference, {10, 20, 30, 40}, 20);
	check(plan.valid);
	check(plan.edges == std::vector<CorpusEdgeKey>({{20, 10}, {20, 30}, {20, 40}}));

	auto missing = buildCorpusPlan(CorpusMode::Reference, {10, 20, 30}, 99);
	check(!missing.valid);
	check(!missing.error.empty());

	std::vector<CorpusBinaryId> largeReference;
	for (CorpusBinaryId id = 0; id < 50; id++)
		largeReference.push_back(id);
	check(buildCorpusPlan(CorpusMode::Reference, largeReference, 0).valid);
}

static void testLineagePlan()
{
	auto adjacent = buildCorpusPlan(CorpusMode::Lineage, {1, 2, 3, 4});
	check(adjacent.valid);
	check(adjacent.edges == std::vector<CorpusEdgeKey>({{1, 2}, {2, 3}, {3, 4}}));

	auto overlay = buildCorpusPlan(CorpusMode::Lineage, {1, 2, 3, 4}, {}, true);
	check(overlay.valid);
	check(overlay.edges == std::vector<CorpusEdgeKey>(
	    {{1, 2}, {2, 3}, {3, 4}, {1, 3}, {1, 4}}));
}

static void testAllPairsPlanAndLimit()
{
	auto plan = buildCorpusPlan(CorpusMode::AllPairs, {4, 3, 2, 1});
	check(plan.valid);
	check(plan.edges == std::vector<CorpusEdgeKey>(
	    {{4, 3}, {4, 2}, {4, 1}, {3, 2}, {3, 1}, {2, 1}}));

	auto ten = buildCorpusPlan(CorpusMode::AllPairs, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
	check(ten.valid && ten.edges.size() == 45);
	auto limited = buildCorpusPlan(CorpusMode::AllPairs,
	    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
	check(!limited.valid);
	check(limited.edges.empty());

	auto duplicate = buildCorpusPlan(CorpusMode::Lineage, {1, 2, 1});
	check(!duplicate.valid);
}

static void testDirectionalMetrics()
{
	std::vector<CorpusMatchSample> matches {
	    {40, 40, 1.0, 1.0, "Exact", "Exact"},
	    {20, 20, 0.8, 0.9, "SmallPrimes", "Changed"}};
	auto metrics = calculateCorpusMetrics(3, 6, 100, 200, matches, 40, 140);
	check(close(metrics.sourceCoverage, 2.0 / 3.0));
	check(close(metrics.targetCoverage, 1.0 / 3.0));
	check(close(metrics.instructionCoverageSource, 0.6));
	check(close(metrics.instructionCoverageTarget, 0.3));
	check(close(metrics.meanSimilarity, 0.9));
	check(close(metrics.medianSimilarity, 0.9));
	check(close(metrics.meanConfidence, 0.95));
	check(metrics.exactMatchCount == 1);
	check(metrics.changedMatchCount == 1);
	check(metrics.matchesByMethod.at("Exact") == 1);

	auto reverse = invertCorpusMetrics(metrics);
	check(close(reverse.sourceCoverage, metrics.targetCoverage));
	check(close(reverse.instructionCoverageSource, metrics.instructionCoverageTarget));
	check(reverse.sourceOnlyInstructionCount == metrics.targetOnlyInstructionCount);
}

static void testZeroFunctionMetrics()
{
	auto metrics = calculateCorpusMetrics(0, 0, 0, 0, {});
	check(close(metrics.sourceCoverage, 0.0));
	check(close(metrics.targetCoverage, 0.0));
	check(close(metrics.meanSimilarity, 0.0));
}

static void testCompletionStateSummary()
{
	auto complete = summarizeCorpusCompletion(
	    {CorpusPairState::Complete, CorpusPairState::Cached, CorpusPairState::Failed});
	check(complete.successful == 2);
	check(complete.failed == 1);
	check(!complete.partial);

	auto cancelled = summarizeCorpusCompletion(
	    {CorpusPairState::Complete, CorpusPairState::Cancelled, CorpusPairState::Queued});
	check(cancelled.successful == 1);
	check(cancelled.cancelled == 1);
	check(cancelled.pending == 1);
	check(cancelled.partial);
}

static FunctionOccurrence occurrence(CorpusBinaryId binary, uint64_t address,
	const std::string& name)
{
	return {binary, address, name, 10};
}

static FamilyLinkEvidence link(FunctionOccurrence source, FunctionOccurrence target,
	double similarity, double confidence, const std::string& method = "Exact")
{
	return {source, target, similarity, confidence, method,
	    {source.binary, target.binary}, similarity >= 0.9995 ? "Exact" : "Changed"};
}

static void testReferenceFamilies()
{
	FunctionOccurrence a1 = occurrence(1, 0x1000, "shared");
	FunctionOccurrence a2 = occurrence(1, 0x2000, "removed");
	FunctionOccurrence b1 = occurrence(2, 0x5000, "shared");
	FunctionOccurrence bOnly = occurrence(2, 0x6000, "target_only");
	FunctionOccurrence c1 = occurrence(3, 0x9000, "shared_changed");
	CorpusPairEvidence ab {{1, 2}, {link(a1, b1, 1.0, 1.0)}, {a2}, {bOnly}};
	CorpusPairEvidence ac {{1, 3}, {link(a1, c1, 0.8, 0.9)}, {a2}, {}};
	auto families = buildReferenceFamilies(1, {1, 2, 3}, {ab, ac});
	check(families.size() == 3);
	check(families[0].members.size() == 3);
	check(families[0].state == FunctionFamilyState::Changed);
	check(families[1].members.size() == 1);
	check(families[1].members.count(1) == 1);
	check(families[1].state == FunctionFamilyState::Unique);
	check(families[2].members.count(2) == 1);
	for (const auto& family : families)
	{
		std::set<CorpusBinaryId> binaries;
		for (const auto& [binary, member] : family.members)
		{
			(void)member;
			check(binaries.insert(binary).second);
		}
	}
}

static void testReferenceConflictKeepsStrongest()
{
	FunctionOccurrence a = occurrence(1, 0x1000, "anchor");
	FunctionOccurrence weak = occurrence(2, 0x2000, "weak");
	FunctionOccurrence strong = occurrence(2, 0x3000, "strong");
	CorpusPairEvidence evidence {{1, 2},
	    {link(a, weak, 0.81, 0.81, "Heuristic"),
	        link(a, strong, 0.95, 0.95, "Structural")}, {}, {}};
	auto families = buildReferenceFamilies(1, {1, 2}, {evidence});
	check(families.size() == 2);
	check(families[0].members.at(2).address == strong.address);
	check(families[0].acceptedLinks.size() == 1);
	check(families[0].rejectedLinks.size() == 1);
	check(families[0].conflicting);
	check(families[0].state == FunctionFamilyState::Conflicting);
}

static void testReferenceTieUsesLowestStableAddress()
{
	FunctionOccurrence anchor = occurrence(1, 0, "anchor");
	FunctionOccurrence first = occurrence(2, 0, "first");
	FunctionOccurrence second = occurrence(2, 1, "second");
	CorpusPairEvidence evidence {{1, 2},
	    {link(anchor, second, 0.9, 0.9, "Structural"),
	        link(anchor, first, 0.9, 0.9, "Structural")}, {}, {}};
	auto families = buildReferenceFamilies(1, {1, 2}, {evidence});
	check(families[0].members.at(2).address == 0);
}

static void testConstrainedFamiliesPreferStrongDirectEvidence()
{
	FunctionOccurrence a1 = occurrence(1, 0x1000, "one");
	FunctionOccurrence a2 = occurrence(1, 0x1100, "other");
	FunctionOccurrence b1 = occurrence(2, 0x2000, "one");
	FunctionOccurrence c1 = occurrence(3, 0x3000, "one");
	FunctionOccurrence c2 = occurrence(3, 0x3100, "weak_other");
	CorpusPairEvidence ab {{1, 2}, {link(a1, b1, 0.92, 0.92)}, {a2}, {}};
	CorpusPairEvidence bc {{2, 3}, {link(b1, c2, 0.60, 0.60, "Heuristic")}, {}, {c1}};
	CorpusPairEvidence ac {{1, 3}, {link(a1, c1, 0.98, 0.98)}, {a2}, {c2}};
	auto families = buildConstrainedFamilies(CorpusMode::Lineage,
	    {1, 2, 3}, {ab, bc, ac});
	auto family = std::find_if(families.begin(), families.end(),
	    [a1](const FunctionFamily& candidate) {
		    auto member = candidate.members.find(1);
		    return member != candidate.members.end() && member->second.address == a1.address;
	    });
	check(family != families.end());
	check(family->members.at(3).address == c1.address);
	check(family->rejectedLinks.size() >= 1);
	for (const auto& candidate : families)
		check(candidate.members.size() <= 3);
}

static void testAllPairsRequiresMutualBestHeuristics()
{
	FunctionOccurrence a = occurrence(1, 0x1000, "a");
	FunctionOccurrence b1 = occurrence(2, 0x2000, "b1");
	FunctionOccurrence b2 = occurrence(2, 0x2100, "b2");
	CorpusPairEvidence alternatives {{1, 2},
	    {link(a, b1, 0.9, 0.9, "Heuristic"),
	        link(a, b2, 0.7, 0.7, "Heuristic")}, {}, {}};
	auto families = buildConstrainedFamilies(CorpusMode::AllPairs,
	    {1, 2}, {alternatives});
	auto accepted = std::find_if(families.begin(), families.end(),
	    [a](const FunctionFamily& family) {
		    auto member = family.members.find(1);
		    return member != family.members.end() && member->second.address == a.address;
	    });
	check(accepted != families.end());
	check(accepted->members.at(2).address == b1.address);
	check(accepted->rejectedLinks.size() == 1);
}

static void testFamilyThresholds()
{
	FunctionOccurrence a = occurrence(1, 0x1000, "a");
	FunctionOccurrence b = occurrence(2, 0x2000, "b");
	CorpusPairEvidence evidence {{1, 2}, {link(a, b, 0.79, 0.9)}, {}, {}};
	FamilyBuildSettings settings;
	settings.minimumSimilarity = 0.8;
	auto families = buildReferenceFamilies(1, {1, 2}, {evidence}, settings);
	check(families.size() == 2);
	check(families[0].members.size() == 1);
	check(families[0].rejectedLinks.size() == 1);
	check(families[1].members.at(2).address == b.address);
}

static void testPatchEvidenceRequiresRegionCoverage()
{
	check(classifyPatchEvidence(0.95, 0.70, 0.2, 1.0)
	    == PatchState::Ambiguous);
	check(classifyPatchEvidence(0.95, 0.70, 1.0, 1.0)
	    == PatchState::VulnerableLike);
	check(classifyPatchEvidence(0.65, 0.92, 1.0, 0.9)
	    == PatchState::FixedLike);
	check(classifyPatchEvidence(0.55, 0.58, 1.0, 1.0)
	    == PatchState::IndependentlyModified);
	check(classifyPatchEvidence(0.90, 0.86, 1.0, 1.0)
	    == PatchState::Ambiguous);
}

static void testConfirmedPatchBlockAbsenceIsEvidence()
{
	auto vulnerable = calculatePatchRegionScore({{true, 0.96}});
	auto fixedAbsent = calculatePatchRegionScore({{true, 0.0}});
	check(close(vulnerable.coverage, 1.0));
	check(close(fixedAbsent.coverage, 1.0));
	check(classifyPatchEvidence(vulnerable.similarity, fixedAbsent.similarity,
	          vulnerable.coverage, fixedAbsent.coverage)
	    == PatchState::VulnerableLike);

	auto vulnerableAbsent = calculatePatchRegionScore({{true, 0.0}});
	auto fixed = calculatePatchRegionScore({{true, 0.94}});
	check(classifyPatchEvidence(vulnerableAbsent.similarity, fixed.similarity,
	          vulnerableAbsent.coverage, fixed.coverage)
	    == PatchState::FixedLike);

	auto unavailable = calculatePatchRegionScore({{false, 0.0}});
	check(close(unavailable.coverage, 0.0));
	check(classifyPatchEvidence(vulnerable.similarity, unavailable.similarity,
	          vulnerable.coverage, unavailable.coverage)
	    == PatchState::Ambiguous);

	// Pure structural patches have no block region on one anchor side. The
	// complementary score turns absence of a fixed-only block into direct
	// vulnerable-like evidence and presence into fixed-like evidence.
	auto addedBlockAbsent = calculatePatchRegionScore({{true, 0.0}});
	auto noVulnerableRegion = complementaryPatchRegionScore(addedBlockAbsent);
	check(classifyPatchEvidence(noVulnerableRegion.similarity,
	          addedBlockAbsent.similarity, noVulnerableRegion.coverage,
	          addedBlockAbsent.coverage)
	    == PatchState::VulnerableLike);
	auto addedBlockPresent = calculatePatchRegionScore({{true, 0.95}});
	noVulnerableRegion = complementaryPatchRegionScore(addedBlockPresent);
	check(classifyPatchEvidence(noVulnerableRegion.similarity,
	          addedBlockPresent.similarity, noVulnerableRegion.coverage,
	          addedBlockPresent.coverage)
	    == PatchState::FixedLike);
}

static void testFunctionLineageStates()
{
	FunctionOccurrence first = occurrence(1, 0x1000, "target");
	FunctionOccurrence same = occurrence(2, 0x2000, "target");
	FunctionOccurrence changed = occurrence(3, 0x3000, "target_changed");
	FunctionOccurrence returned = occurrence(5, 0x5000, "target_returned");
	FunctionFamily family;
	family.members = {{1, first}, {2, same}, {3, changed}, {5, returned}};
	family.acceptedLinks = {link(first, same, 1.0, 1.0),
	    link(same, changed, 0.82, 0.91), link(changed, returned, 0.88, 0.90)};
	std::vector<CorpusBinaryId> order {0, 1, 2, 3, 4, 5};

	check(classifyFunctionLineage(family, order, 0).state
	    == FunctionLineageState::Absent);
	check(classifyFunctionLineage(family, order, 1).state
	    == FunctionLineageState::Introduced);
	auto identical = classifyFunctionLineage(family, order, 2);
	check(identical.state == FunctionLineageState::Identical);
	check(identical.similarity && close(*identical.similarity, 1.0));
	auto modified = classifyFunctionLineage(family, order, 3);
	check(modified.state == FunctionLineageState::Changed);
	check(modified.confidence && close(*modified.confidence, 0.91));
	check(classifyFunctionLineage(family, order, 4).state
	    == FunctionLineageState::Absent);
	check(classifyFunctionLineage(family, order, 5).state
	    == FunctionLineageState::Reappeared);

	PatchClassification patch;
	patch.state = PatchState::VulnerableLike;
	check(classifyFunctionLineage(family, order, 3, &patch).state
	    == FunctionLineageState::VulnerableLike);
	patch.state = PatchState::FixedLike;
	check(classifyFunctionLineage(family, order, 3, &patch).state
	    == FunctionLineageState::FixedLike);
	patch.state = PatchState::IndependentlyModified;
	check(classifyFunctionLineage(family, order, 3, &patch).state
	    == FunctionLineageState::IndependentlyModified);
}

static void testFunctionTransitionStates()
{
	FunctionOccurrence first = occurrence(1, 0x1000, "target");
	FunctionOccurrence same = occurrence(2, 0x2000, "target");
	FunctionOccurrence changed = occurrence(3, 0x3000, "target_changed");
	FunctionOccurrence returned = occurrence(5, 0x5000, "target_returned");
	FunctionFamily family;
	family.members = {{1, first}, {2, same}, {3, changed}, {5, returned}};
	family.acceptedLinks = {link(first, same, 1.0, 1.0),
	    link(same, changed, 0.82, 0.91), link(changed, returned, 0.88, 0.90)};
	std::vector<CorpusBinaryId> order {0, 1, 2, 3, 4, 5};

	check(classifyFunctionTransition(family, order, 0).state
	    == FunctionTransitionState::Introduced);
	auto identical = classifyFunctionTransition(family, order, 1);
	check(identical.state == FunctionTransitionState::Unchanged);
	check(identical.similarity && close(*identical.similarity, 1.0));
	auto modified = classifyFunctionTransition(family, order, 2);
	check(modified.state == FunctionTransitionState::Modified);
	check(modified.confidence && close(*modified.confidence, 0.91));
	check(classifyFunctionTransition(family, order, 3).state
	    == FunctionTransitionState::Removed);
	check(classifyFunctionTransition(family, order, 4).state
	    == FunctionTransitionState::Reappeared);

	PatchClassification vulnerable;
	vulnerable.state = PatchState::VulnerableLike;
	PatchClassification fixed;
	fixed.state = PatchState::FixedLike;
	check(classifyFunctionTransition(family, order, 2, &vulnerable, &fixed).state
	    == FunctionTransitionState::PatchBoundary);
	fixed.state = PatchState::IndependentlyModified;
	check(classifyFunctionTransition(family, order, 2, &vulnerable, &fixed).state
	    == FunctionTransitionState::ClassificationChanged);

	FunctionFamily unresolved;
	unresolved.members = {{1, first}, {2, same}};
	check(classifyFunctionTransition(unresolved, {1, 2}, 0).state
	    == FunctionTransitionState::Unresolved);
}

static void testBinaryCountsFromPairSnapshots()
{
	// IDs are not vector offsets, and pair direction need not match vector order.
	std::vector<CorpusBinary> binaries {
	    {7, "first", "first", 10, 0}, {3, "middle", "middle", 12, 0},
	    {42, "last", "last", 15, 0}, {99, "not compared", "other", 20, 123}};
	CorpusPairMetrics raw;
	raw.sourceFunctionCount = 12;
	raw.targetFunctionCount = 10;
	raw.sourceInstructionCount = 120;
	raw.targetInstructionCount = 100;
	updateCorpusBinaryCounts(binaries, {3, 7}, raw);
	check(binaries[0].instructionCount == 100 && binaries[1].instructionCount == 120);
	check(binaries[2].instructionCount == 0 && binaries[3].instructionCount == 123);
	raw.targetFunctionCount = 15;
	raw.targetInstructionCount = 150;
	updateCorpusBinaryCounts(binaries, {3, 42}, raw);
	check(binaries[1].instructionCount == 120); // Not doubled for a second edge.
	check(binaries[2].functionCount == 15 && binaries[2].instructionCount == 150);
	updateCorpusBinaryCounts(binaries, {3, 42}, raw); // Cached/repeated publication.
	check(binaries[1].instructionCount == 120 && binaries[2].instructionCount == 150);
	updateCorpusBinaryCounts(binaries, {101, 102}, raw);
	check(binaries[3].instructionCount == 123);
}

int main()
{
	testNaturalReleaseOrdering();
	testReferencePlan();
	testLineagePlan();
	testAllPairsPlanAndLimit();
	testDirectionalMetrics();
	testZeroFunctionMetrics();
	testBinaryCountsFromPairSnapshots();
	testCompletionStateSummary();
	testReferenceFamilies();
	testReferenceConflictKeepsStrongest();
	testReferenceTieUsesLowestStableAddress();
	testConstrainedFamiliesPreferStrongDirectEvidence();
	testAllPairsRequiresMutualBestHeuristics();
	testFamilyThresholds();
	testPatchEvidenceRequiresRegionCoverage();
	testConfirmedPatchBlockAbsenceIsEvidence();
	testFunctionLineageStates();
	testFunctionTransitionStates();
	return 0;
}
