#include "patchsignature.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "nlohmann/json.hpp"

using namespace BinaryNinja;

namespace bn_graph_diff {

namespace {

using json = nlohmann::json;

const char* lineOperationName(LineOp operation)
{
	switch (operation)
	{
	case LineOp::Equal: return "equal";
	case LineOp::Rename: return "rename";
	case LineOp::Replace: return "replace";
	case LineOp::Insert: return "insert";
	case LineOp::Delete: return "delete";
	}
	return "unknown";
}

std::string joinedSpans(const std::vector<TokenSpan>& spans)
{
	std::string text;
	for (const auto& span : spans)
		text += span.text;
	return text;
}

std::string changedLineText(const std::vector<TokenSpan>& spans,
	const std::string& renderedText)
{
	return spans.empty() ? renderedText : joinedSpans(spans);
}

void appendUnique(std::vector<std::string>& values, const std::string& value)
{
	if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end())
		values.push_back(value);
}

struct PatchFeatures
{
	std::set<std::string> callTargets;
	std::set<std::string> constants;
	std::set<std::string> stringReferences;
};

bool endsWithControlTargetMarker(std::string text)
{
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
		text.pop_back();
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	auto endsWith = [&text](const char* suffix) {
		const size_t length = std::char_traits<char>::length(suffix);
		return text.size() >= length
		    && text.compare(text.size() - length, length, suffix) == 0;
	};
	return endsWith("goto") || endsWith("then") || endsWith("else")
	    || endsWith("=>");
}

bool branchLabelToken(const json& tokens, size_t index)
{
	if (!tokens.is_array() || index >= tokens.size() || !tokens[index].is_object())
		return false;
	const std::string kind = tokens[index].value("kind", "");
	if (kind == "gotoLabel")
		return true;
	if (kind != "integer" && kind != "possibleValue")
		return false;
	for (size_t next = index + 1; next < tokens.size(); next++)
	{
		if (!tokens[next].is_object())
			continue;
		const std::string text = tokens[next].value("text", "");
		if (text.find_first_not_of(" \t\r\n") == std::string::npos)
			continue;
		if (text.find('@') != std::string::npos)
			return true;
		break;
	}
	for (size_t previous = index; previous > 0; previous--)
	{
		if (!tokens[previous - 1].is_object())
			continue;
		const std::string text = tokens[previous - 1].value("text", "");
		if (text.find_first_not_of(" \t\r\n") == std::string::npos)
			continue;
		return endsWithControlTargetMarker(text);
	}
	return false;
}

bool generatedDataSymbol(std::string text)
{
	while (!text.empty() && text.front() == '_')
		text.erase(text.begin());
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	static const std::vector<std::string> prefixes {
		"jump_table_", "data_", "byte_", "word_", "dword_", "qword_",
		"oword_", "off_", "unk_"};
	for (const auto& prefix : prefixes)
	{
		if (text.rfind(prefix, 0) != 0 || text.size() == prefix.size())
			continue;
		return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
		    text.end(), [](unsigned char value) { return std::isxdigit(value) != 0; });
	}
	return false;
}

PatchFeatures collectPatchFeatures(const ExtractedFunction& function,
	const std::vector<uint64_t>& patchBlocks)
{
	PatchFeatures features;
	std::set<uint64_t> selected(patchBlocks.begin(), patchBlocks.end());
	for (const auto& block : function.blocks)
	{
		if (!selected.count(block.firstAddr))
			continue;
		json lines = json::parse(block.linesJson, nullptr, false);
		if (!lines.is_array())
			continue;
		for (const auto& line : lines)
		{
			const json tokens = line.value("tokens", json::array());
			std::string rendered = line.value("text", "");
			const bool renderFromTokens = rendered.empty();
			for (size_t index = 0; index < tokens.size(); index++)
			{
				const auto& token = tokens[index];
				const std::string kind = token.value("kind", "");
				const std::string text = token.value("text", "");
				if (renderFromTokens)
					rendered += text;
				if (text.empty())
					continue;
				if (kind == "codeSymbol" || kind == "import"
				    || kind == "indirectImport" || kind == "externalSymbol")
					features.callTargets.insert(text);
				else if (kind == "integer" || kind == "floatingPoint")
				{
					if (!branchLabelToken(tokens, index))
						features.constants.insert(text);
				}
				else if (kind == "dataSymbol")
				{
					if (!generatedDataSymbol(text))
					features.stringReferences.insert(text);
				}
			}
			// Some HLIL renderers expose imported callees as plain text tokens.
			// Recover underscore-prefixed call names so patch-core selection does
			// not expand to the entire function merely because token metadata is
			// incomplete.
			for (size_t open = rendered.find('('); open != std::string::npos;
			     open = rendered.find('(', open + 1))
			{
				size_t begin = open;
				while (begin > 0)
				{
					const unsigned char value = rendered[begin - 1];
					if (!std::isalnum(value) && value != '_')
						break;
					begin--;
				}
				if (begin < open && rendered[begin] == '_')
					features.callTargets.insert(rendered.substr(begin, open - begin));
			}
		}
	}
	return features;
}

bool blockContainsAnyCallTarget(const ExtractedBlock& block,
	const std::set<std::string>& targets)
{
	if (targets.empty())
		return false;
	ExtractedFunction singleBlock;
	singleBlock.blocks.push_back(block);
	const auto features = collectPatchFeatures(singleBlock, {block.firstAddr});
	return std::any_of(features.callTargets.begin(), features.callTargets.end(),
	    [&targets](const std::string& target) { return targets.count(target) != 0; });
}

bool blockContainsCallTarget(const ExtractedBlock& block)
{
	ExtractedFunction singleBlock;
	singleBlock.blocks.push_back(block);
	return !collectPatchFeatures(singleBlock, {block.firstAddr}).callTargets.empty();
}

bool securitySensitiveCallTarget(std::string target)
{
	std::transform(target.begin(), target.end(), target.begin(),
	    [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	static const std::vector<std::string> markers {
		"fail", "error", "abort", "panic", "deny", "reject"};
	return std::any_of(markers.begin(), markers.end(),
	    [&target](const std::string& marker) {
		    return target.find(marker) != std::string::npos;
	    });
}

bool blockContainsSecuritySensitiveCall(const ExtractedBlock& block)
{
	ExtractedFunction singleBlock;
	singleBlock.blocks.push_back(block);
	const auto calls = collectPatchFeatures(singleBlock, {block.firstAddr}).callTargets;
	return std::any_of(calls.begin(), calls.end(), securitySensitiveCallTarget);
}

std::optional<size_t> nearestChangedCallBlock(const ExtractedFunction& function,
	const std::vector<size_t>& candidates, double relativePosition)
{
	std::optional<size_t> best;
	double bestDistance = 2.0;
	if (function.blocks.empty())
		return best;
	auto [minimum, maximum] = std::minmax_element(function.blocks.begin(),
	    function.blocks.end(), [](const ExtractedBlock& left,
	                              const ExtractedBlock& right) {
		    return left.firstAddr < right.firstAddr;
	    });
	const double base = minimum->firstAddr;
	const double denominator = std::max<double>(1.0,
	    static_cast<double>(maximum->firstAddr - minimum->firstAddr));
	for (size_t index : candidates)
	{
		if (index >= function.blocks.size()
		    || !blockContainsCallTarget(function.blocks[index]))
			continue;
		const double position = (function.blocks[index].firstAddr - base) / denominator;
		const double distance = std::abs(position - relativePosition);
		if (!best || distance < bestDistance)
		{
			best = index;
			bestDistance = distance;
		}
	}
	return best;
}

double relativeBlockPosition(const ExtractedFunction& function, size_t index)
{
	if (function.blocks.empty() || index >= function.blocks.size())
		return 0.0;
	auto [minimum, maximum] = std::minmax_element(function.blocks.begin(),
	    function.blocks.end(), [](const ExtractedBlock& left,
	                              const ExtractedBlock& right) {
		    return left.firstAddr < right.firstAddr;
	    });
	const double denominator = std::max<double>(1.0,
	    static_cast<double>(maximum->firstAddr - minimum->firstAddr));
	return (function.blocks[index].firstAddr - minimum->firstAddr) / denominator;
}

void narrowPatchRegionToDistinctiveCalls(PatchSignature& signature,
	const ExtractedFunction& vulnerable, const ExtractedFunction& fixed,
	const BlockDiffResult& diff)
{
	const PatchFeatures vulnerableFeatures = collectPatchFeatures(
	    vulnerable, signature.vulnerablePatchBlocks);
	const PatchFeatures fixedFeatures = collectPatchFeatures(
	    fixed, signature.fixedPatchBlocks);
	std::set<std::string> vulnerableOnly;
	std::set<std::string> fixedOnly;
	std::set_difference(vulnerableFeatures.callTargets.begin(),
	    vulnerableFeatures.callTargets.end(), fixedFeatures.callTargets.begin(),
	    fixedFeatures.callTargets.end(), std::inserter(vulnerableOnly, vulnerableOnly.end()));
	std::set_difference(fixedFeatures.callTargets.begin(), fixedFeatures.callTargets.end(),
	    vulnerableFeatures.callTargets.begin(), vulnerableFeatures.callTargets.end(),
	    std::inserter(fixedOnly, fixedOnly.end()));
	std::vector<size_t> vulnerableIndices;
	std::vector<size_t> fixedIndices;
	for (size_t index : signature.vulnerablePatchBlockIndices)
		if (index < vulnerable.blocks.size()
		    && blockContainsAnyCallTarget(vulnerable.blocks[index], vulnerableOnly))
			vulnerableIndices.push_back(index);
	for (size_t index : signature.fixedPatchBlockIndices)
		if (index < fixed.blocks.size()
		    && blockContainsAnyCallTarget(fixed.blocks[index], fixedOnly))
			fixedIndices.push_back(index);
	if (vulnerableIndices.empty() && fixedIndices.empty())
	{
		for (size_t index : diff.onlyA)
			if (index < vulnerable.blocks.size()
			    && blockContainsSecuritySensitiveCall(vulnerable.blocks[index]))
				vulnerableIndices.push_back(index);
		for (size_t index : diff.onlyB)
			if (index < fixed.blocks.size()
			    && blockContainsSecuritySensitiveCall(fixed.blocks[index]))
				fixedIndices.push_back(index);
	}
	if (vulnerableIndices.empty() && fixedIndices.empty())
		return;
	if (vulnerableIndices.empty() && !fixedIndices.empty())
	{
		const double relative = relativeBlockPosition(fixed, fixedIndices.front());
		if (auto nearest = nearestChangedCallBlock(vulnerable,
		        signature.vulnerablePatchBlockIndices, relative))
			vulnerableIndices.push_back(*nearest);
	}
	if (fixedIndices.empty() && !vulnerableIndices.empty())
	{
		const double relative = relativeBlockPosition(
		    vulnerable, vulnerableIndices.front());
		if (auto nearest = nearestChangedCallBlock(fixed,
		        signature.fixedPatchBlockIndices, relative))
			fixedIndices.push_back(*nearest);
	}

	signature.vulnerablePatchBlockIndices = vulnerableIndices;
	signature.fixedPatchBlockIndices = fixedIndices;
	signature.vulnerablePatchBlocks.clear();
	signature.fixedPatchBlocks.clear();
	for (size_t index : vulnerableIndices)
		signature.vulnerablePatchBlocks.push_back(vulnerable.blocks[index].firstAddr);
	for (size_t index : fixedIndices)
		signature.fixedPatchBlocks.push_back(fixed.blocks[index].firstAddr);
}

void appendSetDifference(std::vector<std::string>& output,
	const std::set<std::string>& vulnerable, const std::set<std::string>& fixed)
{
	for (const auto& value : vulnerable)
		if (!fixed.count(value))
			output.push_back("removed: " + value);
	for (const auto& value : fixed)
		if (!vulnerable.count(value))
			output.push_back("added: " + value);
}

std::string renderedLineText(const json& line)
{
	std::string text = line.value("text", "");
	if (!text.empty() || !line.contains("tokens") || !line["tokens"].is_array())
		return text;
	for (const auto& token : line["tokens"])
		if (token.is_object())
			text += token.value("text", "");
	return text;
}

void appendWholeBlockChanges(PatchSignature& signature,
	const ExtractedFunction& function, const std::vector<size_t>& blockIndices,
	bool fixedSide)
{
	const std::string prefix = fixedSide ? "fixed: " : "vulnerable: ";
	for (size_t blockIndex : blockIndices)
	{
		if (blockIndex >= function.blocks.size())
			continue;
		json lines = json::parse(function.blocks[blockIndex].linesJson, nullptr, false);
		if (!lines.is_array())
			continue;
		for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++)
		{
			const auto& line = lines[lineIndex];
			if (!line.is_object())
				continue;
			std::string text = renderedLineText(line);
			if (text.empty())
				continue;
			signature.changedSemanticLines++;
			std::optional<uint64_t> address = lineIndex < function.blocks[blockIndex].lineAddrs.size()
			    ? std::optional<uint64_t>(function.blocks[blockIndex].lineAddrs[lineIndex])
			    : std::nullopt;
			signature.changedLines.push_back({fixedSide ? "insert" : "delete",
			    fixedSide ? std::string {} : text, fixedSide ? text : std::string {},
			    fixedSide ? std::nullopt : address, fixedSide ? address : std::nullopt});
			bool recordedToken = false;
			if (line.contains("tokens") && line["tokens"].is_array())
				for (const auto& token : line["tokens"])
				{
					if (!token.is_object())
						continue;
					std::string tokenText = token.value("text", "");
					if (tokenText.find_first_not_of(" \t\r\n") == std::string::npos)
						continue;
					recordedToken = true;
					signature.changedTokenSpans++;
					appendUnique(signature.changedTokens, prefix + tokenText);
				}
			if (!recordedToken)
			{
				signature.changedTokenSpans++;
				appendUnique(signature.changedTokens, prefix + text);
			}
		}
	}
}

FunctionRef functionAt(BinaryViewRef view, uint64_t address)
{
	if (!view)
		return nullptr;
	for (const auto& function : view->GetAnalysisFunctionsForAddress(address))
		if (function && function->GetStart() == address)
			return function;
	return nullptr;
}

std::optional<FunctionMatchRecord> matchAt(const FunctionMatchMaps& maps,
	uint64_t source, uint64_t target)
{
	auto found = std::find_if(maps.matches.begin(), maps.matches.end(),
	    [source, target](const FunctionMatchRecord& match) {
		    return match.a.address == source && match.b.address == target;
	    });
	return found == maps.matches.end() ? std::nullopt
	                                 : std::optional<FunctionMatchRecord>(*found);
}

FunctionViewType chooseViewType(BinaryViewRef vulnerable, BinaryViewRef fixed)
{
	FunctionViewType hlil(HighLevelILFunctionGraph);
	if (hlil.IsValidForView(vulnerable) && hlil.IsValidForView(fixed))
		return hlil;
	FunctionViewType mlil(MediumLevelILFunctionGraph);
	return mlil.IsValidForView(vulnerable) && mlil.IsValidForView(fixed)
	    ? mlil : FunctionViewType(NormalFunctionGraph);
}

PatchRegionScore scoreRegion(const ExtractedFunction& anchor,
	const std::vector<size_t>& patchBlocks, const BlockDiffResult& diff)
{
	if (patchBlocks.empty())
		return {};
	std::vector<PatchRegionObservation> observations;
	observations.reserve(patchBlocks.size());
	for (size_t block : patchBlocks)
	{
		if (block >= anchor.blocks.size())
		{
			observations.push_back({});
			continue;
		}
		auto pair = std::find_if(diff.pairs.begin(), diff.pairs.end(),
		    [block](const BlockPair& candidate) { return candidate.a == block; });
		if (pair != diff.pairs.end())
		{
			observations.push_back({true, pair->similarity});
			continue;
		}
		const bool confirmedAbsent = std::find(
		    diff.onlyA.begin(), diff.onlyA.end(), block) != diff.onlyA.end();
		observations.push_back({confirmedAbsent, 0.0});
	}
	return calculatePatchRegionScore(observations);
}

void appendChangedBlockAddresses(PatchSignature& signature,
	const ExtractedFunction& vulnerable, const ExtractedFunction& fixed,
	const BlockDiffResult& diff)
{
	std::set<uint64_t> vulnerableBlocks;
	std::set<uint64_t> fixedBlocks;
	std::set<size_t> vulnerableIndices;
	std::set<size_t> fixedIndices;
	for (const auto& pair : diff.pairs)
		if (pair.status == BlockStatus::Changed)
		{
			signature.pairedChangedBlocks++;
			if (pair.a < vulnerable.blocks.size())
			{
				vulnerableIndices.insert(pair.a);
				vulnerableBlocks.insert(vulnerable.blocks[pair.a].firstAddr);
			}
			if (pair.b < fixed.blocks.size())
			{
				fixedIndices.insert(pair.b);
				fixedBlocks.insert(fixed.blocks[pair.b].firstAddr);
			}
		}
	for (size_t index : diff.onlyA)
		if (index < vulnerable.blocks.size())
		{
			signature.removedBlocks++;
			vulnerableIndices.insert(index);
			vulnerableBlocks.insert(vulnerable.blocks[index].firstAddr);
		}
	for (size_t index : diff.onlyB)
		if (index < fixed.blocks.size())
		{
			signature.addedBlocks++;
			fixedIndices.insert(index);
			fixedBlocks.insert(fixed.blocks[index].firstAddr);
		}
	signature.vulnerablePatchBlocks.assign(vulnerableBlocks.begin(), vulnerableBlocks.end());
	signature.fixedPatchBlocks.assign(fixedBlocks.begin(), fixedBlocks.end());
	signature.vulnerablePatchBlockIndices.assign(
	    vulnerableIndices.begin(), vulnerableIndices.end());
	signature.fixedPatchBlockIndices.assign(fixedIndices.begin(), fixedIndices.end());
}

}  // namespace

std::optional<PatchSignature> buildPatchSignature(FunctionFamilyId family,
	BinaryViewRef vulnerableView, const FunctionOccurrence& vulnerable,
	BinaryViewRef fixedView, const FunctionOccurrence& fixed, std::string* error)
{
	auto fail = [error](const std::string& message) -> std::optional<PatchSignature> {
		if (error)
			*error = message;
		return std::nullopt;
	};
	if (!vulnerableView || !fixedView)
		return fail("Both anchor BinaryViews must be open");
	FunctionRef vulnerableFunction = functionAt(vulnerableView, vulnerable.address);
	FunctionRef fixedFunction = functionAt(fixedView, fixed.address);
	if (!vulnerableFunction || !fixedFunction)
		return fail("The selected anchor function is no longer available");
	auto maps = DiffEngine::instance().cachedFunctionMatchesRef(vulnerableView, fixedView);
	if (!maps)
		return fail("Run the selected anchor pair before creating a patch signature");
	auto record = matchAt(*maps, vulnerable.address, fixed.address);
	if (!record)
		return fail("The selected anchors are not an accepted function pair");
	FunctionViewType viewType = chooseViewType(vulnerableView, fixedView);
	auto extractedVulnerable = DiffEngine::instance().extract(vulnerableFunction, viewType);
	auto extractedFixed = DiffEngine::instance().extract(fixedFunction, viewType);
	if (!extractedVulnerable || !extractedFixed)
		return fail("The selected IL has no graph for one of the anchors");
	auto diff = DiffEngine::instance().blockDiff(
	    *extractedVulnerable, *extractedFixed, *maps);
	if (!diff)
		return fail("The selected anchor diff could not be computed");

	PatchSignature signature;
	signature.family = family;
	signature.vulnerable = vulnerable;
	signature.fixed = fixed;
	signature.wholeFunctionSimilarity = record->similarity;
	signature.confidence = record->confidence;
	signature.viewType = viewType;
	signature.extractionSchemaVersion = DiffEngine::extractionSchemaVersion();
	signature.matchSchemaVersion = DiffEngine::matchSchemaVersion();
	signature.vulnerableBinaryIdentity =
	    DiffEngine::instance().persistentIdentity(vulnerableView).value_or("transient");
	signature.fixedBinaryIdentity =
	    DiffEngine::instance().persistentIdentity(fixedView).value_or("transient");
	appendChangedBlockAddresses(signature, *extractedVulnerable, *extractedFixed, *diff);
	narrowPatchRegionToDistinctiveCalls(
	    signature, *extractedVulnerable, *extractedFixed, *diff);
	const std::set<size_t> selectedVulnerable(
	    signature.vulnerablePatchBlockIndices.begin(),
	    signature.vulnerablePatchBlockIndices.end());
	const std::set<size_t> selectedFixed(signature.fixedPatchBlockIndices.begin(),
	    signature.fixedPatchBlockIndices.end());
	for (const auto& [pair, lineDiff] : diff->lineDiffs)
	{
		if (!selectedVulnerable.count(pair.first) && !selectedFixed.count(pair.second))
			continue;
		const ExtractedBlock* vulnerableBlock = pair.first < extractedVulnerable->blocks.size()
		    ? &extractedVulnerable->blocks[pair.first] : nullptr;
		const ExtractedBlock* fixedBlock = pair.second < extractedFixed->blocks.size()
		    ? &extractedFixed->blocks[pair.second] : nullptr;
		size_t vulnerableLine = 0;
		size_t fixedLine = 0;
		for (const auto& line : lineDiff.lines)
		{
			const bool semantic = line.op == LineOp::Replace
			    || line.op == LineOp::Insert || line.op == LineOp::Delete;
			if (semantic)
			{
				std::optional<uint64_t> vulnerableAddress;
				std::optional<uint64_t> fixedAddress;
				if (line.op != LineOp::Insert && vulnerableBlock
				    && vulnerableLine < vulnerableBlock->lineAddrs.size())
					vulnerableAddress = vulnerableBlock->lineAddrs[vulnerableLine];
				if (line.op != LineOp::Delete && fixedBlock
				    && fixedLine < fixedBlock->lineAddrs.size())
					fixedAddress = fixedBlock->lineAddrs[fixedLine];
				signature.changedSemanticLines++;
				signature.changedLines.push_back({lineOperationName(line.op),
				    changedLineText(line.aSpans, line.aText),
				    changedLineText(line.bSpans, line.bText),
				    vulnerableAddress, fixedAddress});
			}
			for (const auto& span : line.aSpans)
				if (semantic && span.changed)
				{
					signature.changedTokenSpans++;
					appendUnique(signature.changedTokens, "vulnerable: " + span.text);
				}
			for (const auto& span : line.bSpans)
				if (semantic && span.changed)
				{
					signature.changedTokenSpans++;
					appendUnique(signature.changedTokens, "fixed: " + span.text);
				}
			if (semantic && line.aSpans.empty() && !line.aText.empty())
			{
				signature.changedTokenSpans++;
				appendUnique(signature.changedTokens, "vulnerable: " + line.aText);
			}
			if (semantic && line.bSpans.empty() && !line.bText.empty())
			{
				signature.changedTokenSpans++;
				appendUnique(signature.changedTokens, "fixed: " + line.bText);
			}
			if (line.op != LineOp::Insert)
				vulnerableLine++;
			if (line.op != LineOp::Delete)
				fixedLine++;
		}
	}
	std::vector<size_t> selectedOnlyA;
	std::vector<size_t> selectedOnlyB;
	std::copy_if(diff->onlyA.begin(), diff->onlyA.end(),
	    std::back_inserter(selectedOnlyA), [&selectedVulnerable](size_t index) {
		    return selectedVulnerable.count(index) != 0;
	    });
	std::copy_if(diff->onlyB.begin(), diff->onlyB.end(),
	    std::back_inserter(selectedOnlyB), [&selectedFixed](size_t index) {
		    return selectedFixed.count(index) != 0;
	    });
	appendWholeBlockChanges(signature, *extractedVulnerable, selectedOnlyA, false);
	appendWholeBlockChanges(signature, *extractedFixed, selectedOnlyB, true);
	if (signature.changedSemanticLines == 0)
		return fail("The selected anchors have no semantic IL changes");
	PatchFeatures vulnerableFeatures = collectPatchFeatures(
	    *extractedVulnerable, signature.vulnerablePatchBlocks);
	PatchFeatures fixedFeatures = collectPatchFeatures(
	    *extractedFixed, signature.fixedPatchBlocks);
	appendSetDifference(signature.changedCallTargets,
	    vulnerableFeatures.callTargets, fixedFeatures.callTargets);
	appendSetDifference(signature.changedConstants,
	    vulnerableFeatures.constants, fixedFeatures.constants);
	appendSetDifference(signature.changedStringReferences,
	    vulnerableFeatures.stringReferences, fixedFeatures.stringReferences);
	signature.securityRelevance = assessSecurityRelevance(signature);
	if (signature.vulnerablePatchBlocks.empty()
	    && signature.fixedPatchBlocks.empty())
		return fail("The anchor pair has no changed region to classify");
	return signature;
}

PatchClassification evaluatePatchTarget(const PatchSignature& signature,
	BinaryViewRef vulnerableView, BinaryViewRef fixedView,
	BinaryViewRef targetView, const std::optional<FunctionOccurrence>& target,
	double minimumScoreMargin, double minimumPatchCoverage)
{
	PatchClassification classification;
	classification.binary = target ? target->binary : 0;
	if (!patchSignatureCompatible(signature))
	{
		classification.state = PatchState::NotAnalyzed;
		classification.evidence.push_back(
		    "The patch signature was produced by an incompatible engine schema");
		return classification;
	}
	if (!target)
	{
		classification.state = PatchState::Absent;
		classification.evidence.push_back("No occurrence exists in this function family");
		return classification;
	}
	classification.occurrence = *target;
	if (target->binary == signature.vulnerable.binary
	    && target->address == signature.vulnerable.address)
	{
		classification.state = PatchState::VulnerableLike;
		classification.vulnerableSimilarity = 1.0;
		classification.patchRegionCoverage = 1.0;
		classification.evidence.push_back("This occurrence is the vulnerable anchor");
		return classification;
	}
	if (target->binary == signature.fixed.binary
	    && target->address == signature.fixed.address)
	{
		classification.state = PatchState::FixedLike;
		classification.fixedSimilarity = 1.0;
		classification.patchRegionCoverage = 1.0;
		classification.evidence.push_back("This occurrence is the fixed anchor");
		return classification;
	}
	if (!vulnerableView || !fixedView || !targetView)
	{
		classification.state = PatchState::NotAnalyzed;
		classification.evidence.push_back("An anchor or target BinaryView is closed");
		return classification;
	}
	auto vulnerableMaps = DiffEngine::instance().cachedFunctionMatchesRef(
	    vulnerableView, targetView);
	auto fixedMaps = DiffEngine::instance().cachedFunctionMatchesRef(
	    fixedView, targetView);
	if (!vulnerableMaps || !fixedMaps)
	{
		classification.state = PatchState::NotAnalyzed;
		classification.evidence.push_back("Both anchor-to-target pair results are required");
		return classification;
	}
	if (!matchAt(*vulnerableMaps, signature.vulnerable.address, target->address)
	    || !matchAt(*fixedMaps, signature.fixed.address, target->address))
	{
		classification.state = PatchState::NotAnalyzed;
		classification.evidence.push_back(
		    "The cached anchor pairs do not map both anchors to this target occurrence");
		return classification;
	}
	FunctionRef vulnerableFunction = functionAt(vulnerableView, signature.vulnerable.address);
	FunctionRef fixedFunction = functionAt(fixedView, signature.fixed.address);
	FunctionRef targetFunction = functionAt(targetView, target->address);
	if (!vulnerableFunction || !fixedFunction || !targetFunction)
	{
		classification.state = PatchState::NotAnalyzed;
		classification.evidence.push_back("One selected function is unavailable");
		return classification;
	}
	auto extractedVulnerable = DiffEngine::instance().extract(
	    vulnerableFunction, signature.viewType);
	auto extractedFixed = DiffEngine::instance().extract(fixedFunction, signature.viewType);
	auto extractedTarget = DiffEngine::instance().extract(targetFunction, signature.viewType);
	if (!extractedVulnerable || !extractedFixed || !extractedTarget)
	{
		classification.state = PatchState::NotAnalyzed;
		classification.evidence.push_back("The signature IL is unavailable in this target");
		return classification;
	}
	auto vulnerableDiff = DiffEngine::instance().blockDiff(
	    *extractedVulnerable, *extractedTarget, *vulnerableMaps);
	auto fixedDiff = DiffEngine::instance().blockDiff(
	    *extractedFixed, *extractedTarget, *fixedMaps);
	if (!vulnerableDiff || !fixedDiff)
	{
		classification.state = PatchState::NotAnalyzed;
		classification.evidence.push_back("Patch-region comparison failed");
		return classification;
	}
	PatchRegionScore vulnerableScore = scoreRegion(
	    *extractedVulnerable, signature.vulnerablePatchBlockIndices, *vulnerableDiff);
	PatchRegionScore fixedScore = scoreRegion(
	    *extractedFixed, signature.fixedPatchBlockIndices, *fixedDiff);
	if (signature.vulnerablePatchBlockIndices.empty())
		vulnerableScore = complementaryPatchRegionScore(fixedScore);
	if (signature.fixedPatchBlockIndices.empty())
		fixedScore = complementaryPatchRegionScore(vulnerableScore);
	classification.vulnerableSimilarity = vulnerableScore.similarity;
	classification.fixedSimilarity = fixedScore.similarity;
	classification.scoreMargin = vulnerableScore.similarity - fixedScore.similarity;
	classification.patchRegionCoverage = std::min(
	    vulnerableScore.coverage, fixedScore.coverage);
	classification.state = classifyPatchEvidence(vulnerableScore.similarity,
	    fixedScore.similarity, vulnerableScore.coverage, fixedScore.coverage,
	    minimumScoreMargin, minimumPatchCoverage);
	classification.evidence.push_back("Vulnerable patch-region similarity "
	    + std::to_string(vulnerableScore.similarity));
	classification.evidence.push_back("Fixed patch-region similarity "
	    + std::to_string(fixedScore.similarity));
	classification.evidence.push_back("Patch-region coverage "
	    + std::to_string(classification.patchRegionCoverage));
	return classification;
}

}  // namespace bn_graph_diff
