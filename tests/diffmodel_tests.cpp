#include "diffmodel.h"

#include <cstdlib>
#include <map>
#include <string>

static void check(bool condition)
{
	if (!condition)
		std::abort();
}

static void testChangeCursorStartsAtFirstChange()
{
	std::optional<size_t> cursor;
	check(advanceChangeCursor(cursor, 3, true) == 0);
	check(advanceChangeCursor(cursor, 3, true) == 1);
	check(advanceChangeCursor(cursor, 3, false) == 0);
}

static void testPreviousStartsAtLastChange()
{
	std::optional<size_t> cursor;
	check(advanceChangeCursor(cursor, 4, false) == 3);
	check(advanceChangeCursor(cursor, 4, false) == 2);
}

static void testOverlayKeysIncludeViewIdentity()
{
	std::map<OverlayBlockKey, int> overlays;
	overlays[{1, 0x1000, 0x1010}] = 1;
	overlays[{2, 0x1000, 0x1010}] = 2;
	check(overlays.size() == 2);
	const OverlayBlockKey a {1, 0x1000, 0x1010};
	const OverlayBlockKey b {2, 0x1000, 0x1010};
	check(overlays[a] == 1);
	check(overlays[b] == 2);

	std::map<OverlayLineKey, int> lines;
	lines[{1, 0x1000, 0x1010, 0x1014, 0}] = 1;
	lines[{1, 0x1000, 0x1010, 0x1014, 1}] = 2;
	check(lines.size() == 2);

	std::map<OverlayLinearLineKey, int> linearLines;
	linearLines[{1, 0x1000, 0x1014, 7}] = 1;
	linearLines[{2, 0x1000, 0x1014, 7}] = 2;
	linearLines[{1, 0x1000, 0x1014, 8}] = 3;
	check(linearLines.size() == 3);
}

static void testPendingMatchPairsAreDirectionIndependent()
{
	check(canonicalViewPair(0x1000, 0x2000) == canonicalViewPair(0x2000, 0x1000));
	check(canonicalViewPair(0x1000, 0x2000).first == 0x1000);
	check(canonicalViewPair(0x1000, 0x2000).second == 0x2000);
}

static void testSecurityRelevantStates()
{
	check(securityAssessmentIsRelevant("Strong", 0.0));
	check(securityAssessmentIsRelevant("Conflicting", 0.0));
	check(securityAssessmentIsRelevant("Possible", 0.39));
	check(!securityAssessmentIsRelevant("Possible", 0.385));
	check(!securityAssessmentIsRelevant("Unknown", 1.0));
	check(securityAssessmentIsRelevant("Possible", 0.13, 0.405));
	check(securityAssessmentIsRelevant("Possible", 0.26, 0.37));
	check(!securityAssessmentIsRelevant("Possible", 0.0, 0.79));
	check(!securityAssessmentIsRelevant("Possible", 0.13, 0.295));
}

static void testCounterpartsKeepBaselineDirection()
{
	// The same address can name different functions on the two sides. A
	// candidate navigation must use the inverse correspondence, not an A lookup.
	const std::map<uint64_t, uint64_t> matches {{0x1000, 0x2000}, {0x2000, 0x3000}};
	check(counterpartAddress(matches, 0x2000, false) == 0x3000);
	check(counterpartAddress(matches, 0x2000, true) == 0x1000);
	check(counterpartAddress(matches, 0x3000, true) == 0x2000);
	check(!counterpartAddress(matches, 0x1000, true));
	check(!counterpartAddress(matches, 0x3000, false));
	check(!counterpartAddress({}, 0x1000, true));
}

static void testSecurityCacheIdentityIsDirectional()
{
	const std::string forward = directionalMatchCacheMaterial(16, "old", "new");
	const std::string reverse = directionalMatchCacheMaterial(16, "new", "old");
	check(forward != reverse);
	check(forward.find("old\nnew") != std::string::npos);
}

static SymbolPortCandidate candidate(uint64_t a, const std::string& nameA,
    uint64_t b, const std::string& nameB, double confidence = 1.0)
{
	SymbolPortCandidate value;
	value.addressA = a;
	value.addressB = b;
	value.functionAExists = true;
	value.functionBExists = true;
	value.shortNameA = value.fullNameA = value.rawNameA = nameA;
	value.shortNameB = value.fullNameB = value.rawNameB = nameB;
	value.confidence = confidence;
	return value;
}

static void testGeneratedFunctionNames()
{
	check(isGeneratedFunctionName("sub_1000"));
	check(isGeneratedFunctionName("FUN_ABCDEF"));
	check(isGeneratedFunctionName("j__memcpy"));
	check(isGeneratedFunctionName("nullsub_3"));
	check(isGeneratedFunctionName("fcn.00401000"));
	check(isGeneratedFunctionName(""));
	check(!isGeneratedFunctionName("sub_parse_header"));
	check(!isGeneratedFunctionName("parse_header"));
}

static void testSymbolPortPlansBothDirections()
{
	std::vector<SymbolPortCandidate> candidates {
	    candidate(0x1000, "parse_header", 0x8000, "sub_8000")};
	SymbolPortOptions options;
	auto toB = buildSymbolPortPlan(candidates, options);
	check(toB.count() == 1);
	check(toB.renames[0].sourceAddress == 0x1000);
	check(toB.renames[0].targetAddress == 0x8000);
	check(toB.renames[0].newShortName == "parse_header");

	options.direction = SymbolPortDirection::BToA;
	auto toA = buildSymbolPortPlan(candidates, options);
	check(toA.count() == 0);
	check(toA.skippedCount(SymbolPortSkipReason::NoSourceSymbol) == 1);
}

static void testSymbolPortProtectsNamesAndWeakMatches()
{
	std::vector<SymbolPortCandidate> candidates {
	    candidate(0x1000, "source_name", 0x8000, "analyst_name"),
	    candidate(0x2000, "weak_name", 0x9000, "sub_9000", 0.4),
	    candidate(0x3000, "sub_3000", 0xa000, "sub_a000")};
	SymbolPortOptions options;
	options.minConfidence = 0.9;
	auto safe = buildSymbolPortPlan(candidates, options);
	check(safe.count() == 0);
	check(safe.skippedCount(SymbolPortSkipReason::TargetAlreadyNamed) == 1);
	check(safe.skippedCount(SymbolPortSkipReason::BelowConfidence) == 1);
	check(safe.skippedCount(SymbolPortSkipReason::NoSourceSymbol) == 1);

	options.overwrite = true;
	auto overwrite = buildSymbolPortPlan(candidates, options);
	check(overwrite.count() == 1);
	check(overwrite.renames[0].oldName == "analyst_name");
}

static void testSymbolPortRejectsMissingAndDuplicateTargets()
{
	SymbolPortCandidate missing = candidate(0x1000, "one", 0x8000, "sub_8000");
	missing.functionBExists = false;
	auto duplicate = candidate(0x2000, "two", 0x8000, "sub_8000");
	auto first = candidate(0x3000, "three", 0x8000, "sub_8000");
	auto plan = buildSymbolPortPlan({missing, first, duplicate}, {});
	check(plan.count() == 1);
	check(plan.skippedCount(SymbolPortSkipReason::MissingFunction) == 1);
	check(plan.skippedCount(SymbolPortSkipReason::DuplicateTarget) == 1);
}

int main()
{
	testChangeCursorStartsAtFirstChange();
	testPreviousStartsAtLastChange();
	testOverlayKeysIncludeViewIdentity();
	testPendingMatchPairsAreDirectionIndependent();
	testCounterpartsKeepBaselineDirection();
	testSecurityRelevantStates();
	testSecurityCacheIdentityIsDirectional();
	testGeneratedFunctionNames();
	testSymbolPortPlansBothDirections();
	testSymbolPortProtectsNamesAndWeakMatches();
	testSymbolPortRejectsMissingAndDuplicateTargets();
	return 0;
}
