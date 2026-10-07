#pragma once

#include <optional>
#include <functional>

#include "patchsignature.h"

namespace bn_graph_diff {

struct CorpusManifest
{
	uint32_t schemaVersion = 1;
	std::vector<CorpusBinary> binaries;
	CorpusPlan plan;
	std::map<CorpusEdgeKey, CorpusPairState> pairStates;
	std::map<CorpusEdgeKey, std::string> pairErrors;
	FamilyBuildSettings familySettings;
	CorpusMetricFilters metricFilters;
	std::optional<PatchSignature> patchSignature;
	std::map<CorpusBinaryId, PatchClassification> patchClassifications;
	double patchMinimumScoreMargin = 0.1;
	double patchMinimumCoverage = 0.6;
	uint64_t lastRunElapsedMicroseconds = 0;
	uint64_t lastRunStartingResidentBytes = 0;
	uint64_t lastRunPeakResidentBytes = 0;
	bool partial = false;
};

std::optional<std::string> corpusManifestPath(const CorpusManifest& manifest);
bool saveCorpusManifest(const std::string& path, const CorpusManifest& manifest,
	std::string* error = nullptr);
std::optional<CorpusManifest> loadCorpusManifest(const std::string& path,
	std::string* error = nullptr);

bool exportCorpusJson(const std::string& path, const CorpusManifest& manifest,
	const std::map<CorpusEdgeKey, CorpusPairResultRef>& pairs,
	const std::vector<FunctionFamily>& families,
	std::string* error = nullptr, const std::function<bool()>& cancelled = {});
bool exportCorpusCsv(const std::string& path, const CorpusManifest& manifest,
	const std::map<CorpusEdgeKey, CorpusPairResultRef>& pairs,
	const std::vector<FunctionFamily>& families,
	std::string* error = nullptr, const std::function<bool()>& cancelled = {});

}  // namespace bn_graph_diff
