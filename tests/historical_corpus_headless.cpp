#include "binaryninjaapi.h"
#include "corpusmodel.h"
#include "diffengine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <string>
#include <vector>

using namespace BinaryNinja;
using namespace bn_graph_diff;
using json = nlohmann::json;

namespace {

struct LoadedSample
{
	std::string path;
	std::string name;
	BinaryViewRef view;
	CorpusBinary metadata;
};

bool expect(bool condition, const std::string& message)
{
	if (!condition)
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
	return condition;
}

std::string architectureName(BinaryViewRef view)
{
	auto architecture = view ? view->GetDefaultArchitecture() : nullptr;
	return architecture ? architecture->GetName() : std::string("unknown");
}

json functionJson(const FunctionSummary& function)
{
	return {{"name", function.name}, {"address", function.address},
	    {"size", function.size}, {"instructions", function.instructionCount}};
}

CorpusPairMetrics metricsFor(const FunctionMatchMaps& maps)
{
	std::vector<CorpusMatchSample> samples;
	samples.reserve(maps.matches.size());
	size_t sourceInstructions = 0;
	size_t targetInstructions = 0;
	for (const auto& match : maps.matches)
	{
		sourceInstructions += match.a.instructionCount;
		targetInstructions += match.b.instructionCount;
		samples.push_back({match.a.instructionCount, match.b.instructionCount,
		    match.similarity, match.confidence, match.matchType, match.contentMatch});
	}
	size_t sourceOnlyInstructions = 0;
	for (const auto& function : maps.unmatchedA)
	{
		sourceInstructions += function.instructionCount;
		sourceOnlyInstructions += function.instructionCount;
	}
	size_t targetOnlyInstructions = 0;
	for (const auto& function : maps.unmatchedB)
	{
		targetInstructions += function.instructionCount;
		targetOnlyInstructions += function.instructionCount;
	}
	return calculateCorpusMetrics(maps.matches.size() + maps.unmatchedA.size(),
	    maps.matches.size() + maps.unmatchedB.size(), sourceInstructions,
	    targetInstructions, samples, sourceOnlyInstructions, targetOnlyInstructions);
}

CorpusPairEvidence familyEvidence(CorpusEdgeKey edge, const FunctionMatchMaps& maps)
{
	CorpusPairEvidence evidence;
	evidence.edge = edge;
	for (const auto& match : maps.matches)
	{
		FunctionOccurrence source {edge.source, match.a.address,
		    match.a.name, match.a.instructionCount};
		FunctionOccurrence target {edge.target, match.b.address,
		    match.b.name, match.b.instructionCount};
		evidence.links.push_back({source, target, match.similarity,
		    match.confidence, match.matchType, edge, match.contentMatch});
	}
	for (const auto& function : maps.unmatchedA)
		evidence.unmatchedSource.push_back({edge.source, function.address,
		    function.name, function.instructionCount});
	for (const auto& function : maps.unmatchedB)
		evidence.unmatchedTarget.push_back({edge.target, function.address,
		    function.name, function.instructionCount});
	return evidence;
}

json pairJson(const LoadedSample& source, const LoadedSample& target,
	const FunctionMatchMaps& maps, const CorpusPairMetrics& metrics)
{
	json result {
	    {"source", source.name},
	    {"target", target.name},
	    {"accepted_matches", maps.matches.size()},
	    {"restored_from_disk", maps.restoredFromDisk},
	    {"source_only", maps.unmatchedA.size()},
	    {"target_only", maps.unmatchedB.size()},
	    {"source_function_coverage", metrics.sourceCoverage},
	    {"target_function_coverage", metrics.targetCoverage},
	    {"source_instruction_coverage", metrics.instructionCoverageSource},
	    {"target_instruction_coverage", metrics.instructionCoverageTarget},
	    {"mean_similarity", metrics.meanSimilarity},
	    {"median_similarity", metrics.medianSimilarity},
	    {"mean_confidence", metrics.meanConfidence},
	    {"matches_by_method", metrics.matchesByMethod},
	    {"matches", json::array()},
	    {"unmatched_source", json::array()},
	    {"unmatched_target", json::array()}
	};
	for (const auto& match : maps.matches)
		result["matches"].push_back({
		    {"source", functionJson(match.a)},
		    {"target", functionJson(match.b)},
		    {"similarity", match.similarity},
		    {"confidence", match.confidence},
		    {"method", match.matchType},
		    {"cfg_similarity", match.cfgSimilarity},
		    {"instruction_similarity", match.instructionSimilarity}
		});
	for (const auto& function : maps.unmatchedA)
		result["unmatched_source"].push_back(functionJson(function));
	for (const auto& function : maps.unmatchedB)
		result["unmatched_target"].push_back(functionJson(function));
	return result;
}

int run(const std::vector<std::string>& paths, const std::string& reportPath)
{
	std::vector<LoadedSample> samples;
	for (size_t index = 0; index < paths.size(); index++)
	{
		BinaryViewRef view = Load(paths[index], true);
		if (!expect(view && view->GetTypeName() != "Raw",
		        "load analyzed sample " + paths[index]))
			return 1;
		LoadedSample sample;
		sample.path = paths[index];
		sample.name = std::filesystem::path(paths[index]).filename().string();
		sample.view = view;
		sample.metadata.id = static_cast<CorpusBinaryId>(index);
		sample.metadata.displayName = sample.name;
		sample.metadata.persistentIdentity = paths[index];
		sample.metadata.functionCount = view->GetAnalysisFunctionList().size();
		samples.push_back(std::move(sample));
	}

	DiffEngine& engine = DiffEngine::instance();
	if (!expect(engine.available(), "load Rust diff engine: " + engine.loadError()))
		return 1;

	std::vector<CorpusBinaryId> order;
	for (const auto& sample : samples)
		order.push_back(sample.metadata.id);
	CorpusPlan plan = buildCorpusPlan(CorpusMode::AllPairs, order);
	if (!expect(plan.valid && plan.edges.size() == 3, "build three-edge all-pairs plan"))
		return 1;

	json report {
	    {"schema", "binja-diff-historical-corpus-v1"},
	    {"mode", "all-pairs"},
	    {"inputs", json::array()},
	    {"pairs", json::array()}
	};
	for (const auto& sample : samples)
		report["inputs"].push_back({
		    {"name", sample.name},
		    {"path", sample.path},
		    {"view_type", sample.view->GetTypeName()},
		    {"architecture", architectureName(sample.view)},
		    {"functions", sample.metadata.functionCount},
		    {"bytes", std::filesystem::file_size(sample.path)}
		});

	std::vector<CorpusPairEvidence> familyPairs;
	size_t exactMatches = 0;
	size_t changedMatches = 0;
	for (const CorpusEdgeKey& edge : plan.edges)
	{
		LoadedSample& source = samples[edge.source];
		LoadedSample& target = samples[edge.target];
		FunctionMatchMaps maps = engine.matchFunctions(source.view, target.view);
		if (!expect(maps.valid,
		        "match " + source.name + " to " + target.name + ": " + maps.error))
			return 1;
		if (!expect(engine.cachedFunctionMatchesRef(source.view, target.view) != nullptr,
		        "cache completed pair " + source.name + " to " + target.name))
			return 1;
		CorpusPairMetrics metrics = metricsFor(maps);
		familyPairs.push_back(familyEvidence(edge, maps));
		exactMatches += std::count_if(maps.matches.begin(), maps.matches.end(),
		    [](const FunctionMatchRecord& match) { return match.contentMatch != "Changed"; });
		changedMatches += std::count_if(maps.matches.begin(), maps.matches.end(),
		    [](const FunctionMatchRecord& match) { return match.contentMatch == "Changed"; });
		report["pairs"].push_back(pairJson(source, target, maps, metrics));
		std::printf("PAIR %s -> %s: %zu accepted, %.1f%% / %.1f%% instruction coverage, "
		            "%.1f%% mean similarity\n",
		    source.name.c_str(), target.name.c_str(), maps.matches.size(),
		    metrics.instructionCoverageSource * 100.0,
		    metrics.instructionCoverageTarget * 100.0,
		    metrics.meanSimilarity * 100.0);
	}

	std::vector<FunctionFamily> families = buildConstrainedFamilies(
	    CorpusMode::AllPairs, order, familyPairs);
	std::map<std::string, size_t> familyStates;
	for (const auto& family : families)
		familyStates[functionFamilyStateName(family.state)]++;
	report["family_count"] = families.size();
	report["family_states"] = familyStates;
	report["exact_match_count"] = exactMatches;
	report["changed_match_count"] = changedMatches;

	std::ofstream output(reportPath, std::ios::binary | std::ios::trunc);
	if (!expect(output.good(), "open report path " + reportPath))
		return 1;
	output << report.dump(2) << '\n';
	output.close();
	if (!expect(output.good(), "write report " + reportPath))
		return 1;
	if (!expect(exactMatches > 0, "observe at least one exact function match")
	    || !expect(changedMatches > 0, "observe at least one changed function match"))
		return 1;

	for (auto& sample : samples)
		if (sample.view && sample.view->GetFile())
			sample.view->GetFile()->Close();
	std::printf("PASS: %zu families, %zu exact matches, %zu changed matches; report %s\n",
	    families.size(), exactMatches, changedMatches, reportPath.c_str());
	return 0;
}

}  // namespace

int main(int argc, char** argv)
{
	if (argc != 5)
	{
		std::fprintf(stderr,
		    "usage: %s <sample-a> <sample-b> <sample-c> <report.json>\n", argv[0]);
		return 1;
	}
	const std::string bundledPlugins = GetBundledPluginDirectory();
	SetBundledPluginDirectory(bundledPlugins);
	LogToStderr(WarningLog);
	if (!BNIsLicenseValidated())
	{
		const char* environmentLicense = std::getenv("BNLICENSE");
		if (environmentLicense && *environmentLicense)
			BNSetLicense(environmentLicense);
		else
		{
			std::ifstream licenseFile(GetUserDirectory() + "/license.dat", std::ios::binary);
			if (licenseFile)
			{
				std::string license((std::istreambuf_iterator<char>(licenseFile)),
				    std::istreambuf_iterator<char>());
				if (!license.empty())
					BNSetLicense(license.c_str());
			}
		}
	}
	InitPlugins(false);
	std::fprintf(stderr, "Binary Ninja headless product=%s license=%s\n",
	    GetProduct().c_str(), BNIsLicenseValidated() ? "valid" : "invalid");
	if (!BNIsLicenseValidated())
	{
		BNShutdown();
		return 77;
	}
	int result = run({argv[1], argv[2], argv[3]}, argv[4]);
	BNShutdown();
	return result;
}
