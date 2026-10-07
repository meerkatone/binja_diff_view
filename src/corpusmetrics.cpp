#include "corpusengine.h"
#include "diffmodel.h"

namespace bn_graph_diff {
namespace {

bool includeFunction(const FunctionSummary& function,
	const CorpusFunctionFlags& flags,
	const CorpusMetricFilters& filters)
{
	if (function.instructionCount < filters.minimumInstructionCount)
		return false;
	auto found = flags.find(function.address);
	if (found != flags.end())
	{
		if (filters.excludeThunks && found->second.thunk)
			return false;
		if (filters.excludeExternalStubs && found->second.externalStub)
			return false;
	}
	if (filters.excludeSmallGeneratedFunctions
	    && function.instructionCount <= filters.generatedFunctionMaximumInstructions
	    && isGeneratedFunctionName(function.name))
		return false;
	return true;
}

} // namespace

CorpusPairMetrics calculateCorpusPairMetricsWithFlags(const FunctionMatchMaps& maps,
	const CorpusFunctionFlags& sourceFlags, const CorpusFunctionFlags& targetFlags,
	const CorpusMetricFilters& filters)
{
	std::vector<CorpusMatchSample> samples;
	samples.reserve(maps.matches.size());
	size_t sourceInstructions = 0;
	size_t targetInstructions = 0;
	size_t sourceOnlyInstructions = 0;
	size_t targetOnlyInstructions = 0;
	size_t sourceFunctionCount = 0;
	size_t targetFunctionCount = 0;
	for (const auto& match : maps.matches)
	{
		bool exactExcluded = !filters.includeExactMatches && match.contentMatch != "Changed";
		bool includeSource = !exactExcluded
		    && includeFunction(match.a, sourceFlags, filters);
		bool includeTarget = !exactExcluded
		    && includeFunction(match.b, targetFlags, filters);
		if (includeSource)
		{
			sourceFunctionCount++;
			sourceInstructions += match.a.instructionCount;
		}
		if (includeTarget)
		{
			targetFunctionCount++;
			targetInstructions += match.b.instructionCount;
		}
		if (includeSource && !includeTarget) sourceOnlyInstructions += match.a.instructionCount;
		if (includeTarget && !includeSource) targetOnlyInstructions += match.b.instructionCount;
		if (!includeSource || !includeTarget)
			continue;
		samples.push_back({match.a.instructionCount, match.b.instructionCount,
		    match.similarity, match.confidence, match.matchType, match.contentMatch});
	}
	for (const auto& function : maps.unmatchedA)
		if (includeFunction(function, sourceFlags, filters))
		{
			sourceFunctionCount++;
			sourceInstructions += function.instructionCount;
			sourceOnlyInstructions += function.instructionCount;
		}
	for (const auto& function : maps.unmatchedB)
		if (includeFunction(function, targetFlags, filters))
		{
			targetFunctionCount++;
			targetInstructions += function.instructionCount;
			targetOnlyInstructions += function.instructionCount;
		}

	return calculateCorpusMetrics(sourceFunctionCount, targetFunctionCount,
	    sourceInstructions, targetInstructions, samples,
	    sourceOnlyInstructions, targetOnlyInstructions);
}

} // namespace bn_graph_diff
