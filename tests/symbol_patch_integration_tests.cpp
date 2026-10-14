#include "binaryninjaapi.h"
#include "diffengine.h"
#include "diffmodel.h"
#include "patchsignature.h"
#include "symbolport.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>

using namespace BinaryNinja;
using namespace bn_graph_diff;

namespace {

bool expect(bool condition, const std::string& message)
{
	if (!condition)
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
	return condition;
}

bool testQualifiedSymbolDisplayName()
{
	const std::string qualifiedName =
	    "NRemoteNotify_Server::TRemoteNotifyServer::RefreshNotifications";
	Ref<Symbol> symbol = new Symbol(
	    FunctionSymbol, qualifiedName, qualifiedName, qualifiedName, 0);
	return expect(functionMatchName({}, symbol) == qualifiedName,
	           "retain qualified identity for function matching")
	    && expect(functionDisplayName({}, symbol) == "RefreshNotifications",
	        "show only the leaf identifier in function labels");
}

bool testSecurityRelevantStates()
{
	return expect(securityAssessmentIsRelevant("Strong", 0.0), "Strong is security-relevant")
	    && expect(securityAssessmentIsRelevant("Possible", 0.39),
	        "Possible with three direct dimensions is security-relevant")
	    && expect(!securityAssessmentIsRelevant("Possible", 0.26),
	        "weak or propagated Possible evidence stays outside Relevant")
	    && expect(securityAssessmentIsRelevant("Conflicting", 0.0),
	        "Conflicting remains in Relevant for explicit review")
	    && expect(!securityAssessmentIsRelevant("Unknown", 1.0),
	        "Unknown is not security-relevant");
}

FunctionRef findFunctionByName(BinaryViewRef view, const std::string& needle)
{
	for (const auto& function : view->GetAnalysisFunctionList())
	{
		Ref<Symbol> symbol = function ? function->GetSymbol() : nullptr;
		if (symbol && symbol->GetShortName().find(needle) != std::string::npos)
			return function;
	}
	return nullptr;
}

FunctionRef findFunctionAt(BinaryViewRef view, uint64_t address)
{
	for (const auto& function : view->GetAnalysisFunctionList())
		if (function && function->GetStart() == address)
			return function;
	return nullptr;
}

std::string functionName(FunctionRef function)
{
	Ref<Symbol> symbol = function ? function->GetSymbol() : nullptr;
	return symbol ? symbol->GetShortName() : std::string();
}

void fillNames(SymbolPortCandidate& candidate, bool sideA, FunctionRef function)
{
	if (sideA)
		candidate.functionAExists = function != nullptr;
	else
		candidate.functionBExists = function != nullptr;
	Ref<Symbol> symbol = function ? function->GetSymbol() : nullptr;
	if (!symbol)
		return;
	if (sideA)
	{
		candidate.shortNameA = symbol->GetShortName();
		candidate.fullNameA = symbol->GetFullName();
		candidate.rawNameA = symbol->GetRawName();
	}
	else
	{
		candidate.shortNameB = symbol->GetShortName();
		candidate.fullNameB = symbol->GetFullName();
		candidate.rawNameB = symbol->GetRawName();
	}
}

bool containsCaseInsensitive(const std::string& value, const std::string& needle)
{
	std::string lowered = value;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
	    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return lowered.find(needle) != std::string::npos;
}

std::string extractedText(const ExtractedFunction& function)
{
	std::string result;
	for (const auto& block : function.blocks)
		result += block.linesJson;
	return result;
}

// Evidence belongs to a function even when counterpart discovery abstains.
bool hasFunctionEvidence(const FunctionMatchMaps& maps, bool sideA,
	uint64_t address, const std::string& label)
{
	for (const auto& match : maps.matches)
		if ((sideA ? match.a.address : match.b.address) == address)
			return std::find(match.securityEvidence.begin(), match.securityEvidence.end(), label)
				!= match.securityEvidence.end();
	for (const auto& function : (sideA ? maps.unmatchedA : maps.unmatchedB))
		if (function.address == address)
			return std::find(function.securityEvidence.begin(), function.securityEvidence.end(), label)
				!= function.securityEvidence.end();
	return false;
}

const FunctionSummary* functionSummary(const FunctionMatchMaps& maps, bool sideA, uint64_t address)
{
	for (const auto& match : maps.matches)
	{
		const auto& function = sideA ? match.a : match.b;
		if (function.address == address) return &function;
	}
	for (const auto& function : (sideA ? maps.unmatchedA : maps.unmatchedB))
		if (function.address == address) return &function;
	return nullptr;
}

int run(const std::string& referencePath, const std::string& strippedPath,
	const std::string& knownSymbolsPath, const std::string& databasePath)
{
	if (!testSecurityRelevantStates())
		return 1;
	BinaryViewRef reference = Load(referencePath, true);
	BinaryViewRef stripped = Load(strippedPath, true);
	if (!expect(reference && reference->GetTypeName() != "Raw", "load symbolized fixture")
	    || !expect(stripped && stripped->GetTypeName() != "Raw", "load stripped fixture"))
		return 1;

	// Some PE/PDB symbols expose a decorated or empty short spelling. The helper
	// keeps normal Binary Ninja short names, but repairs an undecoded MSVC name
	// from its raw ABI spelling without expanding it into a full signature.
	const std::string msvcRaw =
	    "?YAsyncGetRemoteNotifications@@YAJPEAU_RPC_ASYNC_STATE@@PEAX"
	    "PEAPEAU__MIDL_winspool_0021@@@Z";
	Ref<Symbol> msvcSymbol = new Symbol(FunctionSymbol, msvcRaw, msvcRaw, msvcRaw, 0);
	if (!expect(functionMatchName(reference, msvcSymbol) == "YAsyncGetRemoteNotifications",
	        "repair decorated Windows short name"))
		return 1;
	FunctionRef source = findFunctionByName(reference, "copy_record");
	if (!expect(source != nullptr, "symbolized fixture exposes copy_record"))
		return 1;

	DiffEngine& engine = DiffEngine::instance();
	if (!expect(engine.available(), "load bundled Rust diff engine: " + engine.loadError()))
		return 1;
	engine.invalidateMatchPair(reference, stripped);
	engine.invalidateMatchPair(stripped, reference);
	std::promise<void> matchStart;
	auto matchSignal = matchStart.get_future().share();
	std::vector<std::future<FunctionMatchMapsRef>> concurrentMatches;
	for (int i = 0; i < 4; ++i)
		concurrentMatches.push_back(std::async(std::launch::async, [&engine, reference, stripped, matchSignal]() {
			matchSignal.wait();
			return engine.matchFunctionsRef(reference, stripped);
		}));
	matchStart.set_value();
	auto initialSnapshot = concurrentMatches.front().get();
	for (size_t i = 1; i < concurrentMatches.size(); ++i)
		if (!expect(concurrentMatches[i].get() == initialSnapshot,
		        "concurrent requests return the same current pair object"))
			return 1;
	FunctionMatchMaps maps = *initialSnapshot;
	if (!expect(maps.valid, "whole-binary matching succeeds: " + maps.error))
		return 1;
	if (!expect(!engine.cachedFunctionMatchesRef(stripped, reference),
	        "reverse direction does not reuse forward security assessment"))
		return 1;
	FunctionMatchMaps reverseMaps = engine.matchFunctions(stripped, reference);
	if (!expect(reverseMaps.valid && reverseMaps.matchCount == maps.matchCount,
	        "reverse direction computes and caches its own assessment"))
		return 1;
	if (!expect(hasFunctionEvidence(reverseMaps, false, source->GetStart(),
				"added string context: success/error semantics"),
			"added string context follows a function-referenced data pointer")
		|| !expect(hasFunctionEvidence(maps, true, source->GetStart(),
				"removed string context: success/error semantics"),
			"reversing the comparison reverses the string-context evidence"))
		return 1;

	FunctionRef symbolSource = findFunctionByName(reference, "symbol_port_fixture");
	if (!expect(symbolSource != nullptr, "symbolized fixture exposes the unchanged helper"))
		return 1;
	auto matched = std::find_if(maps.matches.begin(), maps.matches.end(),
		[&](const FunctionMatchRecord& record) { return record.a.address == symbolSource->GetStart(); });
	if (!expect(matched != maps.matches.end(), "matcher pairs the stripped unchanged helper"))
		return 1;
	Ref<Symbol> sourceSymbol = symbolSource->GetSymbol();
	if (!expect(sourceSymbol
			&& matched->a.name == functionMatchName(reference, sourceSymbol),
			"match results use Binary Ninja symbol short names"))
		return 1;
	FunctionRef target = findFunctionAt(stripped, matched->b.address);
	if (!expect(target != nullptr, "matched target function exists"))
		return 1;
	const std::string generatedName = functionName(target);
	if (!expect(isGeneratedFunctionName(generatedName),
			"target starts stripped (got '" + generatedName + "')"))
		return 1;

	// This is the same compiled target before stripping, so its local symbols
	// provide independent ground truth for selected-function comparison.
	BinaryViewRef knownSymbols = Load(knownSymbolsPath, true);
	FunctionRef knownHelper = knownSymbols ? findFunctionByName(knownSymbols, "symbol_port_fixture") : nullptr;
	if (!expect(knownHelper && knownHelper->GetStart() == target->GetStart(),
			"automatic helper pairing agrees with the fixture's independent symbol addresses"))
		return 1;
	FunctionRef knownCopy = knownSymbols ? findFunctionByName(knownSymbols, "copy_record") : nullptr;
	FunctionRef changedTarget = knownCopy ? findFunctionAt(stripped, knownCopy->GetStart()) : nullptr;
	const auto* beforeCopy = changedTarget
		? functionSummary(maps, false, changedTarget->GetStart()) : nullptr;
	const auto* afterCopy = functionSummary(maps, true, source->GetStart());
	if (!expect(knownCopy && changedTarget && beforeCopy && afterCopy,
			"resolve the changed wrapper from the fixture's unstripped symbols"))
		return 1;
	FunctionOccurrence vulnerableOccurrence {0, changedTarget->GetStart(), functionName(changedTarget),
		beforeCopy->instructionCount};
	FunctionOccurrence fixedOccurrence {1, source->GetStart(), functionName(source),
		afterCopy->instructionCount};

	// Patch signatures require an accepted pair. Register an explicit Manual
	// fixture correspondence when discovery abstains; never relabel it as an
	// automatic match or accept a conflicting automatic correspondence.
	auto expectedReverse = engine.cachedFunctionMatchesRef(stripped, reference);
	if (!expect(expectedReverse != nullptr, "retain reverse pair for fixture acceptance"))
		return 1;
	FunctionMatchMaps acceptedReverse = *expectedReverse;
	bool acceptedCopy = false;
	for (const auto& record : acceptedReverse.matches)
		if (record.a.address == changedTarget->GetStart() || record.b.address == source->GetStart())
		{
			if (!expect(record.a.address == changedTarget->GetStart()
					&& record.b.address == source->GetStart(),
					"automatic changed-wrapper pairing does not contradict fixture ground truth"))
				return 1;
			acceptedCopy = true;
		}
	if (!acceptedCopy)
	{
		auto a = std::find_if(acceptedReverse.unmatchedA.begin(), acceptedReverse.unmatchedA.end(),
			[&](const auto& function) { return function.address == changedTarget->GetStart(); });
		auto b = std::find_if(acceptedReverse.unmatchedB.begin(), acceptedReverse.unmatchedB.end(),
			[&](const auto& function) { return function.address == source->GetStart(); });
		if (!expect(a != acceptedReverse.unmatchedA.end() && b != acceptedReverse.unmatchedB.end(),
				"manual fixture correspondence uses two unmatched functions"))
			return 1;
		FunctionMatchRecord manual;
		manual.a = *a;
		manual.b = *b;
		manual.confidence = 1.0;
		manual.matchType = "Manual";
		acceptedReverse.funcMap[manual.a.address] = manual.b.address;
		acceptedReverse.renameMap[manual.a.name] = manual.b.name;
		char from[32], to[32];
		std::snprintf(from, sizeof(from), "0x%llx", (unsigned long long)manual.a.address);
		std::snprintf(to, sizeof(to), "0x%llx", (unsigned long long)manual.b.address);
		acceptedReverse.addrMap[from] = to;
		acceptedReverse.matches.push_back(std::move(manual));
		acceptedReverse.unmatchedA.erase(a);
		acceptedReverse.unmatchedB.erase(b);
		acceptedReverse.matchCount = acceptedReverse.matches.size();
		if (!expect(engine.replaceFunctionMatchesIfCurrent(
				stripped, reference, expectedReverse, acceptedReverse),
				"publish explicit fixture correspondence against the current cache object"))
			return 1;
		std::puts("INFO: changed wrapper unmatched automatically; using Manual fixture correspondence for signature test");
	}
	std::string patchError;
	auto signature = buildPatchSignature(1, stripped, vulnerableOccurrence,
	    reference, fixedOccurrence, &patchError);
	if (!expect(signature.has_value(),
	        "build directional guard patch signature: " + patchError)
	    || !expect(!signature->vulnerablePatchBlocks.empty()
	            || !signature->fixedPatchBlocks.empty(),
	        "guard signature retains its structural patch blocks")
	    || !expect(signature->changedSemanticLines > 0
	            && std::any_of(signature->changedLines.begin(), signature->changedLines.end(),
	                [](const PatchChangedLine& line) {
		                return containsCaseInsensitive(line.fixedText, "length")
		                    || containsCaseInsensitive(line.fixedText, "return");
	                }),
	        "guard signature retains inserted fixed-side semantic text"))
		return 1;

	// Publishing a reverse-direction manual pair invalidates the forward
	// cache object. Obtain a current forward snapshot for the port/save flow.
	auto portSnapshot = engine.matchFunctionsRef(reference, stripped);
	if (!expect(portSnapshot && portSnapshot->valid
			&& portSnapshot->funcMap.count(symbolSource->GetStart())
			&& portSnapshot->funcMap.at(symbolSource->GetStart()) == target->GetStart(),
			"refresh the forward cache after reverse fixture acceptance"))
		return 1;

	SymbolPortCandidate candidate;
	candidate.addressA = symbolSource->GetStart();
	candidate.addressB = target->GetStart();
	candidate.confidence = matched->confidence;
	fillNames(candidate, true, symbolSource);
	fillNames(candidate, false, target);
	SymbolPortOptions options;
	options.direction = SymbolPortDirection::AToB;
	SymbolPortPlan plan = buildSymbolPortPlan({candidate}, options);
	if (!expect(plan.count() == 1, "planner approves unchanged helper A-to-B rename"))
		return 1;

	SymbolPortApplyResult applied = applySymbolPortPlan(stripped, plan);
	if (!expect(applied.success && applied.applied == 1 && applied.changedAfterPreview == 0,
	        "apply symbol port as one transaction")
	    || !expect(functionName(target) == plan.renames[0].newShortName,
	        "target receives source symbol"))
		return 1;

	if (!expect(stripped->CanUndo() && stripped->Undo(), "symbol port is undoable")
	    || !expect(functionName(target) == generatedName, "undo restores generated name")
	    || !expect(stripped->CanRedo() && stripped->Redo(), "symbol port is redoable")
	    || !expect(functionName(target) == plan.renames[0].newShortName,
	        "redo restores ported symbol"))
		return 1;

	// Match the selected-function UI worker: use only retained core references
	// and create independent graphs off the caller thread.
	auto extraction = std::async(std::launch::async, [&engine, source, changedTarget]() {
		return std::make_pair(
		    engine.extract(source, FunctionViewType(MediumLevelILFunctionGraph)),
		    engine.extract(changedTarget, FunctionViewType(MediumLevelILFunctionGraph)));
	}).get();
	auto& extractedReference = extraction.first;
	auto& extractedStripped = extraction.second;
	if (!expect(extractedReference.has_value() && extractedStripped.has_value(),
	        "extract MLIL graphs for the known changed-function pair"))
		return 1;
	const std::string referenceText = extractedText(*extractedReference);
	const std::string strippedText = extractedText(*extractedStripped);
	if (!expect(containsCaseInsensitive(referenceText, "memcpy")
	            && containsCaseInsensitive(strippedText, "memcpy"),
	        "both versions retain the memcpy data path"))
		return 1;

	auto patch = std::async(std::launch::async, [&]() {
		return engine.blockDiff(*extractedReference, *extractedStripped, maps);
	}).get();
	if (!expect(patch.has_value(), "Rust engine produces a block diff"))
		return 1;
	const bool hasChangedPair = std::any_of(patch->pairs.begin(), patch->pairs.end(),
	    [](const BlockPair& pair) { return pair.status == BlockStatus::Changed; });
	const bool hasStructuralDelta = hasChangedPair || !patch->onlyA.empty() || !patch->onlyB.empty();
	if (!expect(hasStructuralDelta && patch->similarity < 0.999,
	        "patch diff detects the removed memcpy length guard"))
		return 1;

	std::filesystem::remove(databasePath);
	if (!expect(stripped->CreateDatabase(databasePath), "save renamed stripped binary as BNDB"))
		return 1;
	// A save changes the durable cache identity. Rewriting the already-computed
	// pair must make it recoverable without another whole-binary extraction.
	auto expectedMatches = engine.cachedFunctionMatchesRef(reference, stripped);
	if (!expect(engine.replaceFunctionMatchesIfCurrent(
	        reference, stripped, expectedMatches, maps),
	        "replace the exact prepared pair after BNDB save"))
		return 1;
	if (!expect(engine.persistCachedFunctionMatches(reference, stripped),
	        "refresh prepared pair after BNDB save"))
		return 1;
	auto prepared = engine.prepareCachedFunctionMatches(reference, stripped);
	if (!expect(prepared.has_value(), "capture prepared pair epochs"))
		return 1;
	engine.invalidateMatches(stripped);
	if (!expect(!engine.preparedFunctionMatchesCurrent(reference, stripped, *prepared),
	        "reject a prepared pair after view invalidation")
	    || !expect(!engine.replaceFunctionMatchesIfCurrent(
	        reference, stripped, expectedMatches, maps),
	        "do not restore an invalidated pair"))
		return 1;
	auto restoredMatches = engine.cachedFunctionMatches(reference, stripped);
	if (!expect(restoredMatches.has_value() && restoredMatches->valid
	            && restoredMatches->restoredFromDisk
	            && restoredMatches->matchCount == maps.matchCount,
	        "restore prepared pair after save without rematching"))
		return 1;

	// A raw binary is reanalyzed on open, so its durable identity must follow the
	// effective analysis settings. A BNDB's saved analysis is covered by its digest.
	const std::string settingsField = "analysis_settings_sha256";
	auto rawIdentity = engine.persistentIdentity(reference);
	auto databaseIdentity = engine.persistentIdentity(stripped);
	if (!expect(rawIdentity && rawIdentity->find(settingsField) != std::string::npos,
	        "raw binary identity includes effective analysis settings")
	    || !expect(databaseIdentity && databaseIdentity->find(settingsField) == std::string::npos,
	        "BNDB identity relies on its saved content"))
		return 1;
	const std::string settingKey = "analysis.limits.maxFunctionSize";
	Ref<Settings> settings = Settings::Instance();
	const uint64_t originalLimit = settings->Get<uint64_t>(settingKey, reference);
	settings->Set(settingKey, originalLimit + 1, reference, SettingsResourceScope);
	auto changedIdentity = engine.persistentIdentity(reference);
	settings->Reset(settingKey, reference, SettingsResourceScope);
	auto restoredIdentity = engine.persistentIdentity(reference);
	if (!expect(changedIdentity && *changedIdentity != *rawIdentity,
	        "changing an analysis setting changes the raw binary identity")
	    || !expect(restoredIdentity == rawIdentity,
	        "restoring the setting restores the raw binary identity"))
		return 1;

	stripped->GetFile()->Close();
	stripped = nullptr;
	target = nullptr;

	BinaryViewRef reopened = Load(databasePath, true);
	FunctionRef persisted = reopened ? findFunctionAt(reopened, matched->b.address) : nullptr;
	if (!expect(reopened != nullptr && persisted != nullptr,
	        "reopen symbol-ported BNDB")
	    || !expect(functionName(persisted) == plan.renames[0].newShortName,
	        "ported symbol persists in BNDB"))
		return 1;

	reopened->GetFile()->Close();
	knownSymbols->GetFile()->Close();
	reference->GetFile()->Close();
	std::filesystem::remove(databasePath);
	std::printf("PASS: matched and ported %s -> %s; patch similarity %.3f\n",
	    generatedName.c_str(), plan.renames[0].newShortName.c_str(), patch->similarity);
	return 0;
}

}  // namespace

int main(int argc, char** argv)
{
	if (argc != 5)
	{
		std::fprintf(stderr, "usage: %s <symbolized> <stripped> <target-with-symbols> <database>\n", argv[0]);
		return 1;
	}
	const std::string coreVersion = GetVersionString();
#ifdef BINJA_DIFF_BN_API_TAG
	const std::string apiTag = BINJA_DIFF_BN_API_TAG;
	const size_t tagSlash = apiTag.rfind('/');
	const std::string apiVersion = tagSlash == std::string::npos ? apiTag : apiTag.substr(tagSlash + 1);
	if (coreVersion.find(apiVersion) == std::string::npos)
	{
		std::fprintf(stderr,
		    "SKIP: Binary Ninja API/core mismatch (headers %s, core %s); "
		    "use the API tag matching the installed core\n",
		    apiVersion.c_str(), coreVersion.c_str());
		return 77;
	}
#endif
	const std::string bundledPlugins = GetBundledPluginDirectory();
	std::fprintf(stderr, "Binary Ninja bundled plugins: %s\n", bundledPlugins.c_str());
	SetBundledPluginDirectory(bundledPlugins);
	LogToStderr(InfoLog);
	// Standalone C++ clients do not always inherit the GUI's validated license.
	// Mirror Binary Ninja's documented headless setup without ever logging the
	// license contents: prefer BNLICENSE (CI), then the normal per-user file.
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
	// BNInitPlugins reports whether every optional plugin initialized, not
	// whether the core loaders are usable. Some headless configurations return
	// false while Mach-O/ELF loading and analysis are fully available.
	// User plugins are unrelated to this integration path and may target another
	// installed Binary Ninja ABI. Load only the bundled architecture and view
	// plugins needed by the fixtures.
	const bool initializedCleanly = InitPlugins(false);
	const bool licenseValidated = BNIsLicenseValidated();
	std::fprintf(stderr, "Binary Ninja plugins initialized (clean=%s, product=%s, license=%s)\n",
	    initializedCleanly ? "true" : "false", GetProduct().c_str(),
	    licenseValidated ? "valid" : "invalid");
	if (!testQualifiedSymbolDisplayName())
	{
		BNShutdown();
		return 1;
	}
	if (!licenseValidated)
	{
		std::fprintf(stderr,
		    "SKIP: Binary Ninja headless license is unavailable; no BinaryView was created\n");
		BNShutdown();
		return 77;
	}
	int result = run(argv[1], argv[2], argv[3], argv[4]);
	BNShutdown();
	return result;
}
