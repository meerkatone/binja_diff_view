#include "corpuspersistence.h"

#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include "nlohmann/json.hpp"

#include <cstdlib>
#include <cstdio>

using namespace bn_graph_diff;

#define check(condition) do { if (!(condition)) { \
	std::fprintf(stderr, "corpus persistence check failed at line %d: %s\n", \
	    __LINE__, #condition); \
	std::abort(); } } while (false)

int main()
{
	QTemporaryDir temporary;
	check(temporary.isValid());
	CorpusManifest manifest;
	manifest.binaries = {{1, "old.bndb", "identity-old", 10, 100, false},
	    {2, "new.bndb", "identity-new", 12, 120, false}};
	manifest.plan = buildCorpusPlan(CorpusMode::Reference, {1, 2}, 1);
	manifest.pairStates[{1, 2}] = CorpusPairState::Complete;
	manifest.metricFilters.minimumInstructionCount = 3;
	manifest.familySettings.minimumSimilarity = 0.7;
	manifest.patchMinimumScoreMargin = 0.2;
	manifest.patchMinimumCoverage = 0.75;
	manifest.lastRunElapsedMicroseconds = 123456;
	manifest.lastRunStartingResidentBytes = 1000;
	manifest.lastRunPeakResidentBytes = 2000;
	PatchSignature signature;
	signature.family = 7;
	signature.vulnerable = {1, 0x1000, "old", 20};
	signature.fixed = {2, 0x2000, "new", 22};
	signature.vulnerablePatchBlocks = {0x1010};
	signature.fixedPatchBlocks = {0x2010};
	signature.vulnerablePatchBlockIndices = {3};
	signature.fixedPatchBlockIndices = {4};
	signature.changedLines = {{"replace", "return 0", "return 1", 0x1014, 0x2014}};
	signature.changedTokens = {"vulnerable: 0", "fixed: 1"};
	signature.changedCallTargets = {"removed: old_check", "added: new_check"};
	signature.changedConstants = {"removed: 0", "added: 1"};
	signature.changedStringReferences = {"added: fixed_message"};
	signature.securityRelevance.state = SecurityRelevanceState::Possible;
	signature.securityRelevance.score = 0.5;
	signature.securityRelevance.evidence = {"test security evidence"};
	signature.viewType = BinaryNinja::FunctionViewType(NormalFunctionGraph);
	signature.extractionSchemaVersion = CurrentExtractionSchemaVersion;
	signature.matchSchemaVersion = CurrentMatchSchemaVersion;
	manifest.patchSignature = signature;
	PatchClassification classification;
	classification.binary = 2;
	classification.occurrence = signature.fixed;
	classification.state = PatchState::FixedLike;
	manifest.patchClassifications[2] = classification;
	auto referencePath = corpusManifestPath(manifest);
	check(referencePath.has_value());
	CorpusManifest lineageManifest = manifest;
	lineageManifest.plan = buildCorpusPlan(CorpusMode::Lineage, {1, 2});
	CorpusManifest overlayManifest = lineageManifest;
	overlayManifest.plan.referenceOverlay = true;
	auto overlayPath = corpusManifestPath(overlayManifest);
	auto lineagePath = corpusManifestPath(lineageManifest);
	check(overlayPath.has_value() && lineagePath.has_value() && overlayPath != lineagePath);
	CorpusManifest dirtyManifest = manifest;
	dirtyManifest.binaries[0].dirty = true;
	check(!corpusManifestPath(dirtyManifest).has_value());

	std::string path = temporary.filePath("manifest.json").toStdString();
	std::string error;
	check(saveCorpusManifest(path, manifest, &error));
	auto restored = loadCorpusManifest(path, &error);
	check(restored.has_value());
	check(restored->binaries.size() == 2);
	check(restored->plan.edges == manifest.plan.edges);
	check(restored->pairStates.at({1, 2}) == CorpusPairState::Complete);
	check(restored->metricFilters.minimumInstructionCount == 3);
	check(restored->familySettings.minimumSimilarity == 0.7);
	check(restored->patchMinimumScoreMargin == 0.2);
	check(restored->patchMinimumCoverage == 0.75);
	check(restored->lastRunElapsedMicroseconds == 123456);
	check(restored->lastRunStartingResidentBytes == 1000);
	check(restored->lastRunPeakResidentBytes == 2000);
	check(restored->patchSignature.has_value());
	check(restored->patchSignature->family == 7);
	check(restored->patchSignature->schemaVersion == PatchSignature::CurrentSchemaVersion);
	check(restored->patchSignature->changedLines.size() == 1);
	check(restored->patchSignature->changedLines[0].fixedText == "return 1");
	check(restored->patchSignature->changedLines[0].vulnerableAddress == 0x1014);
	check(restored->patchSignature->changedLines[0].fixedAddress == 0x2014);
	check(restored->patchSignature->vulnerablePatchBlockIndices
	    == signature.vulnerablePatchBlockIndices);
	check(restored->patchSignature->fixedPatchBlockIndices
	    == signature.fixedPatchBlockIndices);
	check(restored->patchSignature->changedTokens == signature.changedTokens);
	check(restored->patchSignature->changedCallTargets == signature.changedCallTargets);
	check(restored->patchSignature->changedConstants == signature.changedConstants);
	check(restored->patchSignature->changedStringReferences
	    == signature.changedStringReferences);
	check(restored->patchSignature->securityRelevance.state
	    == SecurityRelevanceState::Possible);
	check(restored->patchSignature->securityRelevance.score == 0.5);
	check(restored->patchSignature->securityRelevance.evidence
	    == signature.securityRelevance.evidence);
	check(restored->patchClassifications.at(2).state == PatchState::FixedLike);

	CorpusManifest staleManifest = manifest;
	staleManifest.patchSignature->matchSchemaVersion = CurrentMatchSchemaVersion - 1;
	std::string stalePath = temporary.filePath("stale-manifest.json").toStdString();
	check(saveCorpusManifest(stalePath, staleManifest, &error));
	auto staleRestored = loadCorpusManifest(stalePath, &error);
	check(staleRestored.has_value());
	check(!staleRestored->patchSignature.has_value());
	check(staleRestored->patchClassifications.empty());

	staleManifest = manifest;
	staleManifest.patchSignature->schemaVersion = PatchSignature::CurrentSchemaVersion - 1;
	check(saveCorpusManifest(stalePath, staleManifest, &error));
	staleRestored = loadCorpusManifest(stalePath, &error);
	check(staleRestored.has_value());
	check(!staleRestored->patchSignature.has_value());
	check(staleRestored->patchClassifications.empty());

	std::string jsonPath = temporary.filePath("export.json").toStdString();
	std::string csvPath = temporary.filePath("export.csv").toStdString();
	auto pair = std::make_shared<CorpusPairResult>();
	pair->edge = {1, 2};
	pair->state = CorpusPairState::Complete;
	pair->rawMetrics.sourceCoverage = 0.75;
	pair->metrics.sourceCoverage = 0.5;
	pair->rawMetrics.matchesByMethod["Exact"] = 3;
	pair->metrics.matchesByMethod["Exact"] = 2;
	FunctionFamily family;
	family.id = 7;
	family.state = FunctionFamilyState::Changed;
	family.members[1] = signature.vulnerable;
	family.members[2] = signature.fixed;
	family.acceptedLinks.push_back({signature.vulnerable, signature.fixed,
	    0.8, 0.9, "Structural", {1, 2}, "Changed"});
	family.members[3] = {3, 0x3000, "target", 18};
	check(requiredPatchComparisonEdges(signature, family)
	    == std::vector<CorpusEdgeKey>({{1, 3}, {2, 3}}));
	std::map<CorpusEdgeKey, CorpusPairResultRef> pairs {{{1, 2}, pair}};
	check(exportCorpusJson(jsonPath, manifest, pairs, {family}, &error));
	check(exportCorpusCsv(csvPath, manifest, pairs, {family}, &error));
	check(QFile(QString::fromStdString(jsonPath)).size() > 0);
	check(QFile(QString::fromStdString(csvPath)).size() > 0);
	QFile csv(QString::fromStdString(csvPath));
	check(csv.open(QIODevice::ReadOnly));
	QByteArray csvContents = csv.readAll();
	check(csvContents.contains("persistent_identity"));
	check(csvContents.contains("identity-old"));
	check(csvContents.contains("patch_signature"));
	check(csvContents.contains("fixed_message"));
	check(csvContents.contains("source_coverage"));
	QFile exportedJson(QString::fromStdString(jsonPath));
	check(exportedJson.open(QIODevice::ReadOnly));
	const auto jsonContents = exportedJson.readAll();
	auto parsed = nlohmann::json::parse(jsonContents.toStdString());
	check(parsed.at("families").size() == 1);
	check(parsed.at("families")[0].at("members").size() == 3);
	check(parsed.at("families")[0].at("accepted_links").size() == 1);
	check(parsed.at("pair_results")[0].at("raw_metrics").at("source_coverage") == 0.75);
	exportedJson.close();
	csv.close();
	// Fail after buffered output has begun; the prior destination must survive
	// and QSaveFile must remove its unpublished temporary file.
	const auto filesBefore = QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden);
	FunctionFamily large = family;
	large.members[1].name = std::string(200000, 'x');
	for (bool asJson : {false, true})
	{
		int polls = 0;
		auto cancelDuringWrite = [&]() { return ++polls >= 4; };
		check(!(asJson
		    ? exportCorpusJson(jsonPath, manifest, pairs, {large}, &error, cancelDuringWrite)
		    : exportCorpusCsv(csvPath, manifest, pairs, {large}, &error, cancelDuringWrite)));
		check(polls >= 4 && !error.empty());
		QFile unchanged(QString::fromStdString(asJson ? jsonPath : csvPath));
		check(unchanged.open(QIODevice::ReadOnly));
		check(unchanged.readAll() == (asJson ? jsonContents : csvContents));
		check(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden) == filesBefore);
	}
	check(!exportCorpusJson(temporary.path().toStdString(), manifest, pairs, {family}, &error));
	check(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden) == filesBefore);
	// CSV quoting alone does not stop spreadsheet formula evaluation.
	for (const std::string& prefix : {"=", "+", "-", "@", "\t", "\r", "\n", "  ="})
	{
		CorpusManifest formulaManifest = manifest;
		formulaManifest.binaries[0].displayName = prefix + "1+1";
		std::string formulaPath = temporary.filePath("formula.csv").toStdString();
		check(exportCorpusCsv(formulaPath, formulaManifest, pairs, {family}, &error));
		QFile formulaFile(QString::fromStdString(formulaPath));
		check(formulaFile.open(QIODevice::ReadOnly));
		check(formulaFile.readAll().contains(QByteArray::fromStdString("\"'" + prefix + "1+1\"")));
	}

	PatchSignature guardedCopy;
	guardedCopy.vulnerable = {1, 0x4000, "parse_request", 10};
	guardedCopy.fixed = {2, 0x5000, "parse_request", 12};
	guardedCopy.changedLines = {{"insert", "", "if (length <= buffer_size)",
	    std::nullopt, std::nullopt}};
	guardedCopy.changedCallTargets = {"added: memcpy_s"};
	auto strong = assessSecurityRelevance(guardedCopy);
	check(strong.state == SecurityRelevanceState::Strong);
	check(strong.score >= 0.5);

	PatchSignature advertisedAuth;
	advertisedAuth.vulnerable = {1, 0x4100, "HandleAuthTypeMessage", 10};
	advertisedAuth.fixed = {2, 0x5100, "HandleAuthTypeMessage", 14};
	advertisedAuth.changedLines = {{"insert", "",
	    "if (!advertised_auth_types.contains(selected_type)) reject_client();",
	    std::nullopt, std::nullopt}};
	advertisedAuth.changedStringReferences = {
	    "added: viewer selected auth type which was not advertised"};
	auto authenticationState = assessSecurityRelevance(advertisedAuth);
	check(authenticationState.state == SecurityRelevanceState::Strong);
	check(authenticationState.score >= 0.5);

	PatchSignature removedUnsafeCopy;
	removedUnsafeCopy.vulnerable = {1, 0x4200, "copy_request", 10};
	removedUnsafeCopy.fixed = {2, 0x5200, "copy_request", 8};
	removedUnsafeCopy.changedCallTargets = {"removed: memcpy"};
	auto removedCopyState = assessSecurityRelevance(removedUnsafeCopy);
	check(removedCopyState.state == SecurityRelevanceState::Possible);
	check(removedCopyState.score >= 0.25);

	PatchSignature nameOnly;
	nameOnly.vulnerable = {1, 0x6000, "RSAKeyPair", 10};
	nameOnly.fixed = {2, 0x7000, "RSAKeyPair", 10};
	auto unknown = assessSecurityRelevance(nameOnly);
	check(unknown.state == SecurityRelevanceState::Unknown);
	check(csvContents.contains("matches_by_method:Exact"));
	check(csvContents.contains("Structural"));

	QFile corrupt(QString::fromStdString(path));
	check(corrupt.open(QIODevice::Append));
	corrupt.write("corrupt");
	corrupt.close();
	check(!loadCorpusManifest(path, &error).has_value());
	return 0;
}
