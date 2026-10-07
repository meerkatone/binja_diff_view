// Small, dependency-free pieces of the diff controller model. Kept separate
// so correctness-sensitive state transitions can be unit tested without a
// running Binary Ninja UI.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

inline bool securityAssessmentIsRelevant(std::string_view state, double directScore, double score = 0.0)
{
	return state == "Strong" || state == "Conflicting"
	    || (state == "Possible" && (directScore >= 0.39 || (directScore >= 0.13 && score >= 0.30)));
}

inline std::string directionalMatchCacheMaterial(
	int schema, std::string_view baselineIdentity, std::string_view patchedIdentity)
{
	return "binja-diff-match-cache\n" + std::to_string(schema) + "\n"
	    + std::string(baselineIdentity) + "\n" + std::string(patchedIdentity);
}

// Look up a navigation driver's counterpart without reversing the stored A/B
// match snapshot (or confusing overlapping addresses in the two binaries).
inline std::optional<uint64_t> counterpartAddress(
	const std::map<uint64_t, uint64_t>& baselineToCandidate,
	uint64_t address, bool fromCandidate)
{
	if (fromCandidate)
	{
		for (const auto& [baseline, candidate] : baselineToCandidate)
			if (candidate == address)
				return baseline;
		return std::nullopt;
	}
	auto found = baselineToCandidate.find(address);
	return found == baselineToCandidate.end() ? std::nullopt
	                                        : std::optional<uint64_t>(found->second);
}

struct OverlayBlockKey
{
	uintptr_t viewIdentity = 0;
	uint64_t functionStart = 0;
	uint64_t blockStart = 0;

	bool operator<(const OverlayBlockKey& other) const
	{
		return std::tie(viewIdentity, functionStart, blockStart)
		    < std::tie(other.viewIdentity, other.functionStart, other.blockStart);
	}
};

struct OverlayLineKey
{
	uintptr_t viewIdentity = 0;
	uint64_t functionStart = 0;
	uint64_t blockStart = 0;
	uint64_t lineAddress = 0;
	size_t instructionIndex = 0;

	bool operator<(const OverlayLineKey& other) const
	{
		return std::tie(viewIdentity, functionStart, blockStart, lineAddress, instructionIndex)
		    < std::tie(other.viewIdentity, other.functionStart, other.blockStart,
		        other.lineAddress, other.instructionIndex);
	}
};

// Linear HLIL bodies do not expose basic-block identity. Keep a parallel line
// key so the same ephemeral overlay can be projected into native Linear view.
struct OverlayLinearLineKey
{
	uintptr_t viewIdentity = 0;
	uint64_t functionStart = 0;
	uint64_t lineAddress = 0;
	size_t instructionIndex = 0;

	bool operator<(const OverlayLinearLineKey& other) const
	{
		return std::tie(viewIdentity, functionStart, lineAddress, instructionIndex)
		    < std::tie(other.viewIdentity, other.functionStart,
		        other.lineAddress, other.instructionIndex);
	}
};

// Treat a whole-binary match as an unordered job while it is running. The
// result itself remains directional, but A->B and B->A must not launch two
// expensive extraction/matching workers for the same pair.
inline std::pair<uintptr_t, uintptr_t> canonicalViewPair(uintptr_t a, uintptr_t b)
{
	return a <= b ? std::make_pair(a, b) : std::make_pair(b, a);
}

inline size_t advanceChangeCursor(
    std::optional<size_t>& cursor, size_t changeCount, bool forward)
{
	if (changeCount == 0)
	{
		cursor.reset();
		return 0;
	}
	if (!cursor)
		cursor = forward ? 0 : changeCount - 1;
	else if (forward)
		cursor = (*cursor + 1) % changeCount;
	else
		cursor = (*cursor + changeCount - 1) % changeCount;
	return *cursor;
}

// Pure, dependency-free symbol-port planning. Binary Ninja objects are resolved
// by the controller before this layer is called, which keeps all overwrite and
// confidence decisions independently testable.
enum class SymbolPortDirection
{
	AToB,
	BToA
};

enum class SymbolPortSkipReason
{
	BelowConfidence,
	NoSourceSymbol,
	TargetAlreadyNamed,
	SameName,
	MissingFunction,
	DuplicateTarget
};

struct SymbolPortOptions
{
	SymbolPortDirection direction = SymbolPortDirection::AToB;
	double minConfidence = 0.0;
	bool overwrite = false;
};

struct SymbolPortCandidate
{
	uint64_t addressA = 0;
	uint64_t addressB = 0;
	bool functionAExists = false;
	bool functionBExists = false;
	std::string shortNameA, fullNameA, rawNameA;
	std::string shortNameB, fullNameB, rawNameB;
	double confidence = 0.0;
};

struct SymbolPortRename
{
	uint64_t sourceAddress = 0;
	uint64_t targetAddress = 0;
	std::string oldName;
	std::string newShortName;
	std::string newFullName;
	std::string newRawName;
	double confidence = 0.0;
};

struct SymbolPortPlan
{
	std::vector<SymbolPortRename> renames;
	std::map<SymbolPortSkipReason, size_t> skipped;

	void skip(SymbolPortSkipReason reason) { skipped[reason]++; }
	size_t count() const { return renames.size(); }
	size_t skippedCount(SymbolPortSkipReason reason) const
	{
		auto it = skipped.find(reason);
		return it == skipped.end() ? 0 : it->second;
	}
};

inline bool isGeneratedFunctionName(const std::string& name)
{
	if (name.empty())
		return true;
	// Binary Ninja names jump-only thunks after their destination (for example
	// j__memcpy on Mach-O). It looks meaningful but is analysis-generated and
	// should not block an explicit source symbol from replacing it.
	if (name.rfind("j_", 0) == 0 || name == "unnamed")
		return true;
	size_t separator = name.find('_');
	if (separator == std::string::npos && name.size() > 4 && name[3] == '.')
		separator = 3;
	if (separator == std::string::npos)
		return false;
	std::string prefix = name.substr(0, separator);
	for (char& ch : prefix)
		if (ch >= 'A' && ch <= 'Z')
			ch = static_cast<char>(ch - 'A' + 'a');
	if (prefix != "sub" && prefix != "fun" && prefix != "func"
	    && prefix != "loc" && prefix != "nullsub" && prefix != "fcn")
		return false;
	if (separator + 1 == name.size())
		return false;
	for (size_t i = separator + 1; i < name.size(); i++)
	{
		char ch = name[i];
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')
		        || (ch >= 'A' && ch <= 'F')))
			return false;
	}
	return true;
}

inline SymbolPortPlan buildSymbolPortPlan(
	const std::vector<SymbolPortCandidate>& candidates, const SymbolPortOptions& options)
{
	SymbolPortPlan plan;
	std::set<uint64_t> targetAddresses;
	for (const auto& candidate : candidates)
	{
		if (candidate.confidence < options.minConfidence)
		{
			plan.skip(SymbolPortSkipReason::BelowConfidence);
			continue;
		}

		bool toB = options.direction == SymbolPortDirection::AToB;
		bool sourceExists = toB ? candidate.functionAExists : candidate.functionBExists;
		bool targetExists = toB ? candidate.functionBExists : candidate.functionAExists;
		if (!sourceExists || !targetExists)
		{
			plan.skip(SymbolPortSkipReason::MissingFunction);
			continue;
		}

		const std::string& sourceShort = toB ? candidate.shortNameA : candidate.shortNameB;
		const std::string& sourceFull = toB ? candidate.fullNameA : candidate.fullNameB;
		const std::string& sourceRaw = toB ? candidate.rawNameA : candidate.rawNameB;
		const std::string& targetName = toB ? candidate.shortNameB : candidate.shortNameA;
		if (isGeneratedFunctionName(sourceShort))
		{
			plan.skip(SymbolPortSkipReason::NoSourceSymbol);
			continue;
		}
		if (sourceShort == targetName)
		{
			plan.skip(SymbolPortSkipReason::SameName);
			continue;
		}
		if (!options.overwrite && !isGeneratedFunctionName(targetName))
		{
			plan.skip(SymbolPortSkipReason::TargetAlreadyNamed);
			continue;
		}

		uint64_t sourceAddress = toB ? candidate.addressA : candidate.addressB;
		uint64_t targetAddress = toB ? candidate.addressB : candidate.addressA;
		if (!targetAddresses.insert(targetAddress).second)
		{
			plan.skip(SymbolPortSkipReason::DuplicateTarget);
			continue;
		}
		plan.renames.push_back({sourceAddress, targetAddress, targetName, sourceShort,
		    sourceFull.empty() ? sourceShort : sourceFull,
		    sourceRaw.empty() ? sourceShort : sourceRaw, candidate.confidence});
	}
	return plan;
}
