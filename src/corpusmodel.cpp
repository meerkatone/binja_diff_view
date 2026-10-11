#include "corpusmodel.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <set>
#include <tuple>

namespace bn_graph_diff {

namespace {

struct ReleaseNameToken
{
	bool numeric = false;
	std::string value;
};

std::string releaseStem(const std::string& name)
{
	size_t separator = name.find_last_of("/\\");
	std::string stem = separator == std::string::npos ? name : name.substr(separator + 1);
	std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	static const std::vector<std::string> suffixes {
	    ".bndb", ".bnpr", ".dylib", ".dll", ".exe", ".elf", ".bin"};
	for (const auto& suffix : suffixes)
		if (stem.size() > suffix.size()
		    && stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
		{
			stem.resize(stem.size() - suffix.size());
			break;
		}
	return stem;
}

std::vector<ReleaseNameToken> tokenizeReleaseName(const std::string& name)
{
	std::vector<ReleaseNameToken> tokens;
	std::string stem = releaseStem(name);
	for (size_t start = 0; start < stem.size();)
	{
		bool numeric = std::isdigit(static_cast<unsigned char>(stem[start])) != 0;
		size_t end = start + 1;
		while (end < stem.size()
		    && (std::isdigit(static_cast<unsigned char>(stem[end])) != 0) == numeric)
			end++;
		tokens.push_back({numeric, stem.substr(start, end - start)});
		start = end;
	}
	return tokens;
}

std::string significantDigits(const std::string& value)
{
	size_t first = value.find_first_not_of('0');
	return first == std::string::npos ? "0" : value.substr(first);
}

bool prereleaseRemainder(const std::vector<ReleaseNameToken>& tokens, size_t start)
{
	std::string remainder;
	for (size_t index = start; index < tokens.size(); index++)
		remainder += tokens[index].value;
	static const std::vector<std::string> markers {
	    "alpha", "beta", "preview", "pre", "rc", "dev", "snapshot"};
	return std::any_of(markers.begin(), markers.end(), [&remainder](const std::string& marker) {
		return remainder.find(marker) != std::string::npos;
	});
}

}  // namespace

bool naturalReleaseNameLess(const std::string& left, const std::string& right)
{
	auto leftTokens = tokenizeReleaseName(left);
	auto rightTokens = tokenizeReleaseName(right);
	size_t common = std::min(leftTokens.size(), rightTokens.size());
	for (size_t index = 0; index <= common; index++)
	{
		// After a shared version number, order all prerelease suffixes before
		// the final release and other suffixes. Apply the same rule to two
		// different suffixes; otherwise the comparator can form a cycle.
		if (index && leftTokens[index - 1].numeric && rightTokens[index - 1].numeric)
		{
			bool leftPrerelease = prereleaseRemainder(leftTokens, index);
			bool rightPrerelease = prereleaseRemainder(rightTokens, index);
			if (leftPrerelease != rightPrerelease) return leftPrerelease;
		}
		if (index == common) break;
		const auto& leftToken = leftTokens[index];
		const auto& rightToken = rightTokens[index];
		if (leftToken.numeric && rightToken.numeric)
		{
			std::string leftDigits = significantDigits(leftToken.value);
			std::string rightDigits = significantDigits(rightToken.value);
			if (leftDigits.size() != rightDigits.size())
				return leftDigits.size() < rightDigits.size();
			if (leftDigits != rightDigits)
				return leftDigits < rightDigits;
		}
		else if (leftToken.numeric != rightToken.numeric)
			return leftToken.numeric;
		else if (leftToken.value != rightToken.value)
			return leftToken.value < rightToken.value;
	}
	if (leftTokens.size() != rightTokens.size())
		return leftTokens.size() == common;
	return false;
}

std::vector<size_t> naturalReleaseOrder(const std::vector<std::string>& names)
{
	std::vector<size_t> order(names.size());
	std::iota(order.begin(), order.end(), size_t {0});
	std::stable_sort(order.begin(), order.end(), [&names](size_t left, size_t right) {
		return naturalReleaseNameLess(names[left], names[right]);
	});
	return order;
}

CorpusPlan buildCorpusPlan(CorpusMode mode,
	const std::vector<CorpusBinaryId>& binaryOrder,
	std::optional<CorpusBinaryId> reference, bool referenceOverlay,
	size_t maximumPairs)
{
	CorpusPlan plan;
	plan.mode = mode;
	plan.binaryOrder = binaryOrder;
	plan.reference = reference;
	plan.referenceOverlay = referenceOverlay;

	if (binaryOrder.size() < 2)
	{
		plan.error = "A corpus plan needs at least two distinct binaries";
		return plan;
	}
	std::set<CorpusBinaryId> unique(binaryOrder.begin(), binaryOrder.end());
	if (unique.size() != binaryOrder.size())
	{
		plan.error = "A corpus plan cannot contain duplicate binary identifiers";
		return plan;
	}

	if (mode == CorpusMode::Reference)
	{
		if (!reference || !unique.count(*reference))
		{
			plan.error = "Reference mode needs a reference from the selected binaries";
			return plan;
		}
		for (CorpusBinaryId target : binaryOrder)
			if (target != *reference)
				plan.edges.push_back({*reference, target});
	}
	else if (mode == CorpusMode::Lineage)
	{
		for (size_t i = 1; i < binaryOrder.size(); i++)
			plan.edges.push_back({binaryOrder[i - 1], binaryOrder[i]});
		if (referenceOverlay)
		{
			CorpusBinaryId first = binaryOrder.front();
			for (size_t i = 2; i < binaryOrder.size(); i++)
				plan.edges.push_back({first, binaryOrder[i]});
		}
	}
	else
	{
		for (size_t source = 0; source < binaryOrder.size(); source++)
			for (size_t target = source + 1; target < binaryOrder.size(); target++)
				plan.edges.push_back({binaryOrder[source], binaryOrder[target]});
	}

	if (mode == CorpusMode::AllPairs && plan.edges.size() > maximumPairs)
	{
		plan.error = "The plan contains " + std::to_string(plan.edges.size())
		    + " pairs, above the configured limit of " + std::to_string(maximumPairs);
		plan.edges.clear();
		return plan;
	}
	plan.valid = true;
	return plan;
}

CorpusCompletionSummary summarizeCorpusCompletion(
	const std::vector<CorpusPairState>& states)
{
	CorpusCompletionSummary summary;
	summary.total = states.size();
	for (CorpusPairState state : states)
	{
		switch (state)
		{
		case CorpusPairState::Cached:
		case CorpusPairState::Complete: summary.successful++; break;
		case CorpusPairState::Failed:
		case CorpusPairState::Invalidated: summary.failed++; break;
		case CorpusPairState::Cancelled: summary.cancelled++; break;
		case CorpusPairState::Queued:
		case CorpusPairState::Running: summary.pending++; break;
		}
	}
	summary.partial = summary.cancelled != 0 || summary.pending != 0;
	return summary;
}

CorpusPairMetrics calculateCorpusMetrics(size_t sourceFunctionCount,
	size_t targetFunctionCount, size_t sourceInstructionCount,
	size_t targetInstructionCount, const std::vector<CorpusMatchSample>& matches,
	size_t sourceOnlyInstructionCount, size_t targetOnlyInstructionCount)
{
	CorpusPairMetrics metrics;
	metrics.sourceFunctionCount = sourceFunctionCount;
	metrics.targetFunctionCount = targetFunctionCount;
	metrics.sourceInstructionCount = sourceInstructionCount;
	metrics.targetInstructionCount = targetInstructionCount;
	metrics.acceptedMatchCount = matches.size();
	metrics.sourceOnlyFunctionCount = sourceFunctionCount > matches.size()
	    ? sourceFunctionCount - matches.size() : 0;
	metrics.targetOnlyFunctionCount = targetFunctionCount > matches.size()
	    ? targetFunctionCount - matches.size() : 0;
	metrics.sourceOnlyInstructionCount = sourceOnlyInstructionCount;
	metrics.targetOnlyInstructionCount = targetOnlyInstructionCount;

	size_t matchedSourceInstructions = 0;
	size_t matchedTargetInstructions = 0;
	std::vector<double> similarities;
	similarities.reserve(matches.size());
	for (const auto& match : matches)
	{
		matchedSourceInstructions += match.sourceInstructionCount;
		matchedTargetInstructions += match.targetInstructionCount;
		metrics.meanSimilarity += match.similarity;
		metrics.meanConfidence += match.confidence;
		similarities.push_back(match.similarity);
		metrics.matchesByMethod[match.matchType]++;
		bool unchanged = match.contentMatch.empty()
		    ? match.similarity >= 0.9995 : match.contentMatch != "Changed";
		if (unchanged)
			metrics.exactMatchCount++;
		else
			metrics.changedMatchCount++;
	}

	if (sourceFunctionCount)
		metrics.sourceCoverage = static_cast<double>(matches.size()) / sourceFunctionCount;
	if (targetFunctionCount)
		metrics.targetCoverage = static_cast<double>(matches.size()) / targetFunctionCount;
	if (sourceInstructionCount)
		metrics.instructionCoverageSource =
		    static_cast<double>(matchedSourceInstructions) / sourceInstructionCount;
	if (targetInstructionCount)
		metrics.instructionCoverageTarget =
		    static_cast<double>(matchedTargetInstructions) / targetInstructionCount;
	if (!matches.empty())
	{
		metrics.meanSimilarity /= matches.size();
		metrics.meanConfidence /= matches.size();
		std::sort(similarities.begin(), similarities.end());
		size_t middle = similarities.size() / 2;
		metrics.medianSimilarity = similarities.size() % 2
		    ? similarities[middle]
		    : (similarities[middle - 1] + similarities[middle]) / 2.0;
	}
	return metrics;
}

void updateCorpusBinaryCounts(std::vector<CorpusBinary>& binaries,
	const CorpusEdgeKey& edge, const CorpusPairMetrics& rawMetrics)
{
	for (auto& binary : binaries)
	{
		if (binary.id == edge.source)
		{
			binary.functionCount = rawMetrics.sourceFunctionCount;
			binary.instructionCount = rawMetrics.sourceInstructionCount;
		}
		else if (binary.id == edge.target)
		{
			binary.functionCount = rawMetrics.targetFunctionCount;
			binary.instructionCount = rawMetrics.targetInstructionCount;
		}
	}
}

CorpusPairMetrics invertCorpusMetrics(const CorpusPairMetrics& metrics)
{
	CorpusPairMetrics result = metrics;
	std::swap(result.sourceFunctionCount, result.targetFunctionCount);
	std::swap(result.sourceInstructionCount, result.targetInstructionCount);
	std::swap(result.sourceOnlyFunctionCount, result.targetOnlyFunctionCount);
	std::swap(result.sourceOnlyInstructionCount, result.targetOnlyInstructionCount);
	std::swap(result.sourceCoverage, result.targetCoverage);
	std::swap(result.instructionCoverageSource, result.instructionCoverageTarget);
	return result;
}

namespace {

FunctionOccurrenceKey occurrenceKey(const FunctionOccurrence& occurrence)
{
	return {occurrence.binary, occurrence.address};
}

int matchMethodStrength(const std::string& method)
{
	if (method == "Manual") return 100;
	if (method == "Exact") return 90;
	if (method == "Name") return 80;
	if (method == "MdIndex") return 70;
	if (method == "SmallPrimes") return 60;
	if (method == "Structural") return 50;
	if (method == "CallGraph") return 40;
	if (method == "Heuristic") return 30;
	return 0;
}

bool strongerEvidence(const FamilyLinkEvidence& left,
	const FamilyLinkEvidence& right)
{
	if (left.confidence != right.confidence)
		return left.confidence > right.confidence;
	if (left.similarity != right.similarity)
		return left.similarity > right.similarity;
	int leftStrength = matchMethodStrength(left.matchType);
	int rightStrength = matchMethodStrength(right.matchType);
	if (leftStrength != rightStrength)
		return leftStrength > rightStrength;
	return std::tie(left.source.address, left.target.address,
	           left.source.binary, left.target.binary)
	    < std::tie(right.source.address, right.target.address,
	          right.source.binary, right.target.binary);
}

bool acceptedByThreshold(const FamilyLinkEvidence& link,
	const FamilyBuildSettings& settings)
{
	return link.similarity >= settings.minimumSimilarity
	    && link.confidence >= settings.minimumConfidence;
}

void classifyFamily(FunctionFamily& family, size_t binaryCount)
{
	if (family.conflicting)
		family.state = FunctionFamilyState::Conflicting;
	else if (family.ambiguous)
		family.state = FunctionFamilyState::Ambiguous;
	else if (family.members.size() == 1)
		family.state = FunctionFamilyState::Unique;
	else if (family.members.size() < binaryCount)
		family.state = FunctionFamilyState::Missing;
	else
	{
		bool identical = std::all_of(family.acceptedLinks.begin(),
		    family.acceptedLinks.end(), [](const FamilyLinkEvidence& link) {
			    return link.contentMatch.empty()
			        ? link.similarity >= 0.9995 : link.contentMatch != "Changed";
		    });
		family.state = identical ? FunctionFamilyState::Identical
		                         : FunctionFamilyState::Changed;
	}

	if (family.acceptedLinks.empty())
	{
		family.minimumSimilarity = 0.0;
		family.minimumConfidence = 0.0;
	}
	else
	{
		family.minimumSimilarity = 1.0;
		family.minimumConfidence = 1.0;
		for (const auto& link : family.acceptedLinks)
		{
			family.minimumSimilarity = std::min(family.minimumSimilarity, link.similarity);
			family.minimumConfidence = std::min(family.minimumConfidence, link.confidence);
		}
	}
}

void finalizeFamilies(std::vector<FunctionFamily>& families, size_t binaryCount)
{
	families.erase(std::remove_if(families.begin(), families.end(),
	    [](const FunctionFamily& family) { return family.members.empty(); }), families.end());
	std::sort(families.begin(), families.end(), [](const FunctionFamily& left,
	    const FunctionFamily& right) {
		auto firstMember = [](const FunctionFamily& family) {
			const auto& occurrence = family.members.begin()->second;
			return std::make_pair(occurrence.binary, occurrence.address);
		};
		return firstMember(left) < firstMember(right);
	});
	for (size_t index = 0; index < families.size(); index++)
	{
		families[index].id = static_cast<FunctionFamilyId>(index + 1);
		std::sort(families[index].acceptedLinks.begin(), families[index].acceptedLinks.end(),
		    strongerEvidence);
		std::sort(families[index].rejectedLinks.begin(), families[index].rejectedLinks.end(),
		    strongerEvidence);
		classifyFamily(families[index], binaryCount);
	}
}

}  // namespace

std::vector<FunctionFamily> buildReferenceFamilies(CorpusBinaryId reference,
	const std::vector<CorpusBinaryId>& binaryOrder,
	const std::vector<CorpusPairEvidence>& pairs,
	const FamilyBuildSettings& settings)
{
	std::map<FunctionOccurrenceKey, FunctionOccurrence> referenceOccurrences;
	std::map<FunctionOccurrenceKey, FunctionOccurrence> targetOnly;
	std::vector<FamilyLinkEvidence> links;
	for (const auto& pair : pairs)
	{
		if (pair.edge.source != reference)
			continue;
		for (const auto& link : pair.links)
		{
			referenceOccurrences[occurrenceKey(link.source)] = link.source;
			targetOnly[occurrenceKey(link.target)] = link.target;
			links.push_back(link);
		}
		for (const auto& occurrence : pair.unmatchedSource)
			referenceOccurrences[occurrenceKey(occurrence)] = occurrence;
		for (const auto& occurrence : pair.unmatchedTarget)
			targetOnly[occurrenceKey(occurrence)] = occurrence;
	}

	std::vector<FunctionFamily> families;
	std::map<FunctionOccurrenceKey, size_t> anchorFamilies;
	for (const auto& [key, occurrence] : referenceOccurrences)
	{
		FunctionFamily family;
		family.members[reference] = occurrence;
		anchorFamilies[key] = families.size();
		families.push_back(std::move(family));
	}
	std::sort(links.begin(), links.end(), strongerEvidence);
	for (const auto& link : links)
	{
		auto anchor = anchorFamilies.find(occurrenceKey(link.source));
		if (anchor == anchorFamilies.end())
			continue;
		FunctionFamily& family = families[anchor->second];
		if (!acceptedByThreshold(link, settings))
		{
			family.rejectedLinks.push_back(link);
			continue;
		}
		auto existing = family.members.find(link.target.binary);
		if (existing == family.members.end())
		{
			family.members[link.target.binary] = link.target;
			family.acceptedLinks.push_back(link);
			targetOnly.erase(occurrenceKey(link.target));
			continue;
		}
		if (existing->second.address == link.target.address)
			continue;
		family.rejectedLinks.push_back(link);
		family.ambiguous = true;
		if (link.similarity >= settings.conflictThreshold
		    && link.confidence >= settings.conflictThreshold)
			family.conflicting = true;
	}

	for (const auto& [key, occurrence] : targetOnly)
	{
		(void)key;
		FunctionFamily family;
		family.members[occurrence.binary] = occurrence;
		families.push_back(std::move(family));
	}
	finalizeFamilies(families, binaryOrder.size());
	return families;
}

std::vector<FunctionFamily> buildConstrainedFamilies(CorpusMode mode,
	const std::vector<CorpusBinaryId>& binaryOrder,
	const std::vector<CorpusPairEvidence>& pairs,
	const FamilyBuildSettings& settings)
{
	std::map<FunctionOccurrenceKey, FunctionOccurrence> occurrences;
	std::vector<FamilyLinkEvidence> links;
	std::vector<FamilyLinkEvidence> rejectedByThreshold;
	for (const auto& pair : pairs)
	{
		for (const auto& link : pair.links)
		{
			occurrences[occurrenceKey(link.source)] = link.source;
			occurrences[occurrenceKey(link.target)] = link.target;
			if (acceptedByThreshold(link, settings))
				links.push_back(link);
			else
				rejectedByThreshold.push_back(link);
		}
		for (const auto& occurrence : pair.unmatchedSource)
			occurrences[occurrenceKey(occurrence)] = occurrence;
		for (const auto& occurrence : pair.unmatchedTarget)
			occurrences[occurrenceKey(occurrence)] = occurrence;
	}

	std::vector<FunctionFamily> families;
	std::map<FunctionOccurrenceKey, size_t> familyFor;
	for (const auto& [key, occurrence] : occurrences)
	{
		FunctionFamily family;
		family.members[occurrence.binary] = occurrence;
		familyFor[key] = families.size();
		families.push_back(std::move(family));
	}
	for (const auto& link : rejectedByThreshold)
		families[familyFor.at(occurrenceKey(link.source))].rejectedLinks.push_back(link);
	std::sort(links.begin(), links.end(), strongerEvidence);
	using BestKey = std::tuple<CorpusBinaryId, CorpusBinaryId, FunctionOccurrenceKey>;
	std::map<BestKey, FamilyLinkEvidence> bestFromSource;
	std::map<BestKey, FamilyLinkEvidence> bestFromTarget;
	if (mode == CorpusMode::AllPairs && settings.requireMutualBestHeuristics)
	{
		for (const auto& link : links)
		{
			if (link.matchType != "Heuristic")
				continue;
			BestKey sourceKey {link.origin.source, link.origin.target, occurrenceKey(link.source)};
			BestKey targetKey {link.origin.source, link.origin.target, occurrenceKey(link.target)};
			auto sourceBest = bestFromSource.find(sourceKey);
			if (sourceBest == bestFromSource.end() || strongerEvidence(link, sourceBest->second))
				bestFromSource[sourceKey] = link;
			auto targetBest = bestFromTarget.find(targetKey);
			if (targetBest == bestFromTarget.end() || strongerEvidence(link, targetBest->second))
				bestFromTarget[targetKey] = link;
		}
	}

	for (const auto& link : links)
	{
		size_t sourceIndex = familyFor.at(occurrenceKey(link.source));
		size_t targetIndex = familyFor.at(occurrenceKey(link.target));
		if (mode == CorpusMode::AllPairs && settings.requireMutualBestHeuristics
		    && link.matchType == "Heuristic")
		{
			BestKey sourceKey {link.origin.source, link.origin.target, occurrenceKey(link.source)};
			BestKey targetKey {link.origin.source, link.origin.target, occurrenceKey(link.target)};
			const auto& sourceBest = bestFromSource.at(sourceKey);
			const auto& targetBest = bestFromTarget.at(targetKey);
			bool mutual = occurrenceKey(sourceBest.target) == occurrenceKey(link.target)
			    && occurrenceKey(targetBest.source) == occurrenceKey(link.source);
			if (!mutual)
			{
				families[sourceIndex].rejectedLinks.push_back(link);
				families[targetIndex].rejectedLinks.push_back(link);
				families[sourceIndex].ambiguous = true;
				families[targetIndex].ambiguous = true;
				continue;
			}
		}
		if (sourceIndex == targetIndex)
		{
			families[sourceIndex].acceptedLinks.push_back(link);
			continue;
		}
		FunctionFamily& sourceFamily = families[sourceIndex];
		FunctionFamily& targetFamily = families[targetIndex];
		bool collision = std::any_of(sourceFamily.members.begin(), sourceFamily.members.end(),
		    [&targetFamily](const auto& member) {
			    return targetFamily.members.count(member.first) != 0;
		    });
		if (collision)
		{
			sourceFamily.rejectedLinks.push_back(link);
			targetFamily.rejectedLinks.push_back(link);
			sourceFamily.ambiguous = targetFamily.ambiguous = true;
			if (link.similarity >= settings.conflictThreshold
			    && link.confidence >= settings.conflictThreshold)
				sourceFamily.conflicting = targetFamily.conflicting = true;
			continue;
		}

		sourceFamily.acceptedLinks.push_back(link);
		sourceFamily.acceptedLinks.insert(sourceFamily.acceptedLinks.end(),
		    targetFamily.acceptedLinks.begin(), targetFamily.acceptedLinks.end());
		sourceFamily.rejectedLinks.insert(sourceFamily.rejectedLinks.end(),
		    targetFamily.rejectedLinks.begin(), targetFamily.rejectedLinks.end());
		sourceFamily.ambiguous = sourceFamily.ambiguous || targetFamily.ambiguous;
		sourceFamily.conflicting = sourceFamily.conflicting || targetFamily.conflicting;
		for (const auto& [binary, occurrence] : targetFamily.members)
		{
			sourceFamily.members[binary] = occurrence;
			familyFor[occurrenceKey(occurrence)] = sourceIndex;
		}
		targetFamily.members.clear();
		targetFamily.acceptedLinks.clear();
		targetFamily.rejectedLinks.clear();
	}

	finalizeFamilies(families, binaryOrder.size());
	return families;
}

const char* functionFamilyStateName(FunctionFamilyState state)
{
	switch (state)
	{
	case FunctionFamilyState::Identical: return "Identical";
	case FunctionFamilyState::Changed: return "Changed";
	case FunctionFamilyState::Missing: return "Missing";
	case FunctionFamilyState::Unique: return "Unique";
	case FunctionFamilyState::Ambiguous: return "Ambiguous";
	case FunctionFamilyState::Conflicting: return "Conflicting";
	}
	return "Unknown";
}

FunctionLineageCell classifyFunctionLineage(const FunctionFamily& family,
	const std::vector<CorpusBinaryId>& binaryOrder, size_t position,
	const PatchClassification* patch)
{
	FunctionLineageCell cell;
	if (position >= binaryOrder.size())
		return cell;
	CorpusBinaryId binary = binaryOrder[position];
	auto current = family.members.find(binary);
	if (current == family.members.end())
		return cell;
	cell.occurrence = current->second;
	if (patch)
	{
		switch (patch->state)
		{
		case PatchState::VulnerableLike:
			cell.state = FunctionLineageState::VulnerableLike;
			return cell;
		case PatchState::FixedLike:
			cell.state = FunctionLineageState::FixedLike;
			return cell;
		case PatchState::IndependentlyModified:
			cell.state = FunctionLineageState::IndependentlyModified;
			return cell;
		case PatchState::Ambiguous:
			cell.state = FunctionLineageState::Ambiguous;
			return cell;
		case PatchState::NotAnalyzed:
			cell.state = FunctionLineageState::NotAnalyzed;
			return cell;
		case PatchState::Absent:
			cell.state = FunctionLineageState::Absent;
			cell.occurrence.reset();
			return cell;
		}
	}

	std::optional<size_t> previousPosition;
	for (size_t prior = position; prior > 0; prior--)
		if (family.members.count(binaryOrder[prior - 1]))
		{
			previousPosition = prior - 1;
			break;
		}
	if (!previousPosition)
	{
		cell.state = position == 0 ? FunctionLineageState::Present
		                           : FunctionLineageState::Introduced;
		return cell;
	}
	if (*previousPosition + 1 < position)
	{
		cell.state = FunctionLineageState::Reappeared;
		return cell;
	}

	const FunctionOccurrence& previous = family.members.at(binaryOrder[*previousPosition]);
	const FamilyLinkEvidence* strongest = nullptr;
	for (const auto& link : family.acceptedLinks)
	{
		bool forward = link.source == previous && link.target == current->second;
		bool reverse = link.target == previous && link.source == current->second;
		if ((forward || reverse) && (!strongest
		    || link.confidence > strongest->confidence
		    || (link.confidence == strongest->confidence
		        && link.similarity > strongest->similarity)))
			strongest = &link;
	}
	if (!strongest)
	{
		cell.state = FunctionLineageState::Present;
		return cell;
	}
	cell.similarity = strongest->similarity;
	cell.confidence = strongest->confidence;
	cell.matchType = strongest->matchType;
	bool unchanged = strongest->contentMatch.empty()
	    ? strongest->similarity >= 0.999 : strongest->contentMatch != "Changed";
	cell.state = unchanged
	    ? FunctionLineageState::Identical : FunctionLineageState::Changed;
	return cell;
}

const char* functionLineageStateName(FunctionLineageState state)
{
	switch (state)
	{
	case FunctionLineageState::Absent: return "absent";
	case FunctionLineageState::Present: return "present";
	case FunctionLineageState::Introduced: return "introduced";
	case FunctionLineageState::Reappeared: return "reappeared";
	case FunctionLineageState::Identical: return "identical";
	case FunctionLineageState::Changed: return "changed";
	case FunctionLineageState::VulnerableLike: return "vulnerable-like";
	case FunctionLineageState::FixedLike: return "fixed-like";
	case FunctionLineageState::IndependentlyModified: return "independently modified";
	case FunctionLineageState::Ambiguous: return "ambiguous";
	case FunctionLineageState::NotAnalyzed: return "not analyzed";
	}
	return "absent";
}

FunctionTransitionCell classifyFunctionTransition(const FunctionFamily& family,
	const std::vector<CorpusBinaryId>& binaryOrder, size_t transitionPosition,
	const PatchClassification* sourcePatch, const PatchClassification* targetPatch)
{
	FunctionTransitionCell cell;
	if (transitionPosition + 1 >= binaryOrder.size())
		return cell;

	CorpusBinaryId sourceBinary = binaryOrder[transitionPosition];
	CorpusBinaryId targetBinary = binaryOrder[transitionPosition + 1];
	auto source = family.members.find(sourceBinary);
	auto target = family.members.find(targetBinary);
	if (source != family.members.end())
		cell.source = source->second;
	if (target != family.members.end())
		cell.target = target->second;

	if (!cell.source && !cell.target)
		return cell;
	if (cell.source && !cell.target)
	{
		cell.state = FunctionTransitionState::Removed;
		return cell;
	}
	if (!cell.source && cell.target)
	{
		bool appearedEarlier = false;
		for (size_t position = 0; position < transitionPosition; position++)
			appearedEarlier = appearedEarlier
			    || family.members.count(binaryOrder[position]) != 0;
		cell.state = appearedEarlier ? FunctionTransitionState::Reappeared
		                             : FunctionTransitionState::Introduced;
		return cell;
	}

	const FamilyLinkEvidence* strongest = nullptr;
	for (const auto& link : family.acceptedLinks)
	{
		bool forward = link.source == *cell.source && link.target == *cell.target;
		bool reverse = link.target == *cell.source && link.source == *cell.target;
		if ((forward || reverse) && (!strongest
		    || link.confidence > strongest->confidence
		    || (link.confidence == strongest->confidence
		        && link.similarity > strongest->similarity)))
			strongest = &link;
	}
	if (strongest)
	{
		cell.similarity = strongest->similarity;
		cell.confidence = strongest->confidence;
		cell.matchType = strongest->matchType;
	}

	if (sourcePatch && targetPatch
	    && sourcePatch->state == PatchState::VulnerableLike
	    && targetPatch->state == PatchState::FixedLike)
	{
		cell.state = FunctionTransitionState::PatchBoundary;
		return cell;
	}
	auto comparablePatchState = [](PatchState state) {
		return state != PatchState::Absent && state != PatchState::NotAnalyzed;
	};
	if (sourcePatch && targetPatch && sourcePatch->state != targetPatch->state
	    && comparablePatchState(sourcePatch->state)
	    && comparablePatchState(targetPatch->state))
	{
		cell.state = FunctionTransitionState::ClassificationChanged;
		return cell;
	}
	if (!strongest)
	{
		cell.state = FunctionTransitionState::Unresolved;
		return cell;
	}
	bool unchanged = strongest->contentMatch.empty()
	    ? strongest->similarity >= 0.999 : strongest->contentMatch != "Changed";
	cell.state = unchanged
	    ? FunctionTransitionState::Unchanged : FunctionTransitionState::Modified;
	return cell;
}

const char* functionTransitionStateName(FunctionTransitionState state)
{
	switch (state)
	{
	case FunctionTransitionState::NoOccurrence: return "no occurrence";
	case FunctionTransitionState::Unchanged: return "unchanged";
	case FunctionTransitionState::Modified: return "modified";
	case FunctionTransitionState::Introduced: return "introduced";
	case FunctionTransitionState::Removed: return "removed";
	case FunctionTransitionState::Reappeared: return "reappeared";
	case FunctionTransitionState::Unresolved: return "unresolved";
	case FunctionTransitionState::PatchBoundary: return "vulnerable-like to fixed-like";
	case FunctionTransitionState::ClassificationChanged: return "classification changed";
	}
	return "no occurrence";
}

PatchState classifyPatchEvidence(double vulnerableSimilarity,
	double fixedSimilarity, double vulnerableCoverage, double fixedCoverage,
	double minimumScoreMargin, double minimumPatchCoverage,
	double independentModificationCeiling)
{
	if (vulnerableCoverage < minimumPatchCoverage
	    || fixedCoverage < minimumPatchCoverage)
		return PatchState::Ambiguous;
	double margin = vulnerableSimilarity - fixedSimilarity;
	if (margin >= minimumScoreMargin)
		return PatchState::VulnerableLike;
	if (-margin >= minimumScoreMargin)
		return PatchState::FixedLike;
	if (vulnerableSimilarity < independentModificationCeiling
	    && fixedSimilarity < independentModificationCeiling)
		return PatchState::IndependentlyModified;
	return PatchState::Ambiguous;
}

PatchRegionScore calculatePatchRegionScore(
	const std::vector<PatchRegionObservation>& observations)
{
	if (observations.empty())
		return {};
	size_t evaluated = 0;
	double similarity = 0.0;
	for (const auto& observation : observations)
	{
		if (!observation.evaluated)
			continue;
		evaluated++;
		similarity += std::clamp(observation.similarity, 0.0, 1.0);
	}
	return {evaluated ? similarity / evaluated : 0.0,
	    static_cast<double>(evaluated) / observations.size()};
}

PatchRegionScore complementaryPatchRegionScore(const PatchRegionScore& observed)
{
	return {1.0 - std::clamp(observed.similarity, 0.0, 1.0), observed.coverage};
}

const char* patchStateName(PatchState state)
{
	switch (state)
	{
	case PatchState::VulnerableLike: return "vulnerable-like";
	case PatchState::FixedLike: return "fixed-like";
	case PatchState::IndependentlyModified: return "independently modified";
	case PatchState::Absent: return "absent";
	case PatchState::Ambiguous: return "ambiguous";
	case PatchState::NotAnalyzed: return "not analyzed";
	}
	return "not analyzed";
}

}  // namespace bn_graph_diff
