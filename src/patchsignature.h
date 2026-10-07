#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "corpusengine.h"

namespace bn_graph_diff {

enum class SecurityRelevanceState
{
	Unknown,
	Possible,
	Strong,
	Conflicting
};

struct SecurityRelevanceAssessment
{
	SecurityRelevanceState state = SecurityRelevanceState::Unknown;
	double score = 0.0;
	std::vector<std::string> evidence;
	std::vector<std::string> counterEvidence;
};

const char* securityRelevanceStateName(SecurityRelevanceState state);

struct PatchChangedLine
{
	std::string operation;
	std::string vulnerableText;
	std::string fixedText;
	std::optional<uint64_t> vulnerableAddress;
	std::optional<uint64_t> fixedAddress;
};

struct PatchSignature
{
	static constexpr uint32_t CurrentSchemaVersion = 16;
	uint32_t schemaVersion = CurrentSchemaVersion;
	FunctionFamilyId family = 0;
	std::string vulnerableBinaryIdentity;
	std::string fixedBinaryIdentity;
	FunctionOccurrence vulnerable;
	FunctionOccurrence fixed;
	double wholeFunctionSimilarity = 0.0;
	double confidence = 0.0;
	std::vector<uint64_t> vulnerablePatchBlocks;
	std::vector<uint64_t> fixedPatchBlocks;
	std::vector<size_t> vulnerablePatchBlockIndices;
	std::vector<size_t> fixedPatchBlockIndices;
	size_t pairedChangedBlocks = 0;
	size_t removedBlocks = 0;
	size_t addedBlocks = 0;
	size_t changedSemanticLines = 0;
	size_t changedTokenSpans = 0;
	std::vector<PatchChangedLine> changedLines;
	std::vector<std::string> changedTokens;
	std::vector<std::string> changedCallTargets;
	std::vector<std::string> changedConstants;
	std::vector<std::string> changedStringReferences;
	SecurityRelevanceAssessment securityRelevance;
	BinaryNinja::FunctionViewType viewType;
	int extractionSchemaVersion = 0;
	int matchSchemaVersion = 0;
};

SecurityRelevanceAssessment assessSecurityRelevance(const PatchSignature& signature);

inline bool patchSignatureCompatible(const PatchSignature& signature)
{
	return signature.schemaVersion == PatchSignature::CurrentSchemaVersion
	    && signature.extractionSchemaVersion == CurrentExtractionSchemaVersion
	    && signature.matchSchemaVersion == CurrentMatchSchemaVersion;
}

inline std::vector<CorpusEdgeKey> requiredPatchComparisonEdges(
	const PatchSignature& signature, const FunctionFamily& family)
{
	std::set<CorpusEdgeKey> edges;
	for (const auto& [binary, occurrence] : family.members)
	{
		(void)occurrence;
		if (binary == signature.vulnerable.binary || binary == signature.fixed.binary)
			continue;
		edges.insert({signature.vulnerable.binary, binary});
		edges.insert({signature.fixed.binary, binary});
	}
	return {edges.begin(), edges.end()};
}

std::optional<PatchSignature> buildPatchSignature(FunctionFamilyId family,
	BinaryViewRef vulnerableView, const FunctionOccurrence& vulnerable,
	BinaryViewRef fixedView, const FunctionOccurrence& fixed,
	std::string* error = nullptr);

PatchClassification evaluatePatchTarget(const PatchSignature& signature,
	BinaryViewRef vulnerableView, BinaryViewRef fixedView,
	BinaryViewRef targetView, const std::optional<FunctionOccurrence>& target,
	double minimumScoreMargin = 0.1, double minimumPatchCoverage = 0.6);

}  // namespace bn_graph_diff
