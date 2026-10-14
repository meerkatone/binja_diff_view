#include "corpuspersistence.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <array>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

#include "diskcache.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace bn_graph_diff {

namespace {

constexpr const char* kManifestMagic = "binja-diff-corpus-manifest-v1";

// Bound serialization buffering and check cancellation between file writes.
// QSaveFile keeps an existing destination intact until commit succeeds.
class ExportBuffer : public std::streambuf
{
	QSaveFile& m_file;
	const std::function<bool()>& m_cancelled;
	std::array<char, 65536> m_buffer;

	int sync() override
	{
		if (m_cancelled && m_cancelled())
			throw std::runtime_error("Export cancelled");
		const auto count = pptr() - pbase();
		if (count && m_file.write(pbase(), count) != count)
			throw std::runtime_error(m_file.errorString().toStdString());
		setp(m_buffer.data(), m_buffer.data() + m_buffer.size());
		return 0;
	}
	int_type overflow(int_type character) override
	{
		sync();
		if (!traits_type::eq_int_type(character, traits_type::eof()))
		{
			*pptr() = traits_type::to_char_type(character);
			pbump(1);
		}
		return traits_type::not_eof(character);
	}

  public:
	ExportBuffer(QSaveFile& file, const std::function<bool()>& cancelled)
	    : m_file(file), m_cancelled(cancelled)
	{
		setp(m_buffer.data(), m_buffer.data() + m_buffer.size());
	}
};

template <typename Serialize>
bool writeExport(const std::string& path, std::string* error,
	const std::function<bool()>& cancelled, Serialize serialize)
{
	if (error) error->clear();
	QSaveFile file(QString::fromStdString(path));
	try
	{
		if (cancelled && cancelled()) throw std::runtime_error("Export cancelled");
		if (!file.open(QIODevice::WriteOnly))
			throw std::runtime_error(file.errorString().toStdString());
		ExportBuffer buffer(file, cancelled);
		std::ostream output(&buffer);
		output.exceptions(std::ios::badbit | std::ios::failbit);
		serialize(output);
		output.flush();
		if (cancelled && cancelled()) throw std::runtime_error("Export cancelled");
		if (!file.commit()) throw std::runtime_error(file.errorString().toStdString());
		return true;
	}
	catch (const std::exception& exception)
	{
		file.cancelWriting();
		if (error) *error = exception.what();
		return false;
	}
}

json edgeJson(const CorpusEdgeKey& edge)
{
	return {{"source", edge.source}, {"target", edge.target}};
}

CorpusEdgeKey edgeFromJson(const json& value)
{
	return {value.at("source").get<CorpusBinaryId>(),
	    value.at("target").get<CorpusBinaryId>()};
}

json occurrenceJson(const FunctionOccurrence& occurrence)
{
	return {{"binary", occurrence.binary}, {"address", occurrence.address},
	    {"name", occurrence.name}, {"instruction_count", occurrence.instructionCount}};
}

FunctionOccurrence occurrenceFromJson(const json& value)
{
	return {value.at("binary").get<CorpusBinaryId>(),
	    value.at("address").get<uint64_t>(), value.value("name", ""),
	    value.value("instruction_count", size_t {0})};
}

json filtersJson(const CorpusMetricFilters& filters)
{
	return {{"minimum_instruction_count", filters.minimumInstructionCount},
	    {"exclude_thunks", filters.excludeThunks},
	    {"exclude_external_stubs", filters.excludeExternalStubs},
	    {"exclude_small_generated_functions", filters.excludeSmallGeneratedFunctions},
	    {"generated_function_maximum_instructions",
	        filters.generatedFunctionMaximumInstructions},
	    {"include_exact_matches", filters.includeExactMatches}};
}

CorpusMetricFilters filtersFromJson(const json& value)
{
	CorpusMetricFilters filters;
	filters.minimumInstructionCount = value.value("minimum_instruction_count", size_t {0});
	filters.excludeThunks = value.value("exclude_thunks", false);
	filters.excludeExternalStubs = value.value("exclude_external_stubs", false);
	filters.excludeSmallGeneratedFunctions =
	    value.value("exclude_small_generated_functions", false);
	filters.generatedFunctionMaximumInstructions =
	    value.value("generated_function_maximum_instructions", size_t {4});
	filters.includeExactMatches = value.value("include_exact_matches", true);
	return filters;
}

json patchSignatureJson(const PatchSignature& signature)
{
	json changedLines = json::array();
	for (const auto& line : signature.changedLines)
	{
		json value = {{"operation", line.operation},
		    {"vulnerable_text", line.vulnerableText}, {"fixed_text", line.fixedText}};
		value["vulnerable_address"] = line.vulnerableAddress
		    ? json(*line.vulnerableAddress) : json(nullptr);
		value["fixed_address"] = line.fixedAddress
		    ? json(*line.fixedAddress) : json(nullptr);
		changedLines.push_back(std::move(value));
	}
	return {{"schema_version", signature.schemaVersion}, {"family", signature.family},
	    {"vulnerable_binary_identity", signature.vulnerableBinaryIdentity},
	    {"fixed_binary_identity", signature.fixedBinaryIdentity},
	    {"vulnerable", occurrenceJson(signature.vulnerable)},
	    {"fixed", occurrenceJson(signature.fixed)},
	    {"whole_function_similarity", signature.wholeFunctionSimilarity},
	    {"confidence", signature.confidence},
	    {"vulnerable_patch_blocks", signature.vulnerablePatchBlocks},
	    {"fixed_patch_blocks", signature.fixedPatchBlocks},
	    {"vulnerable_patch_block_indices", signature.vulnerablePatchBlockIndices},
	    {"fixed_patch_block_indices", signature.fixedPatchBlockIndices},
	    {"paired_changed_blocks", signature.pairedChangedBlocks},
	    {"removed_blocks", signature.removedBlocks},
	    {"added_blocks", signature.addedBlocks},
	    {"changed_semantic_lines", signature.changedSemanticLines},
	    {"changed_token_spans", signature.changedTokenSpans},
	    {"changed_lines", std::move(changedLines)},
	    {"changed_tokens", signature.changedTokens},
	    {"changed_call_targets", signature.changedCallTargets},
	    {"changed_constants", signature.changedConstants},
	    {"changed_string_references", signature.changedStringReferences},
	    {"security_relevance",
	        {{"state", static_cast<int>(signature.securityRelevance.state)},
	            {"state_name", securityRelevanceStateName(signature.securityRelevance.state)},
	            {"score", signature.securityRelevance.score},
	            {"evidence", signature.securityRelevance.evidence},
	            {"counter_evidence", signature.securityRelevance.counterEvidence}}},
	    {"view_type", static_cast<int>(signature.viewType.type)},
	    {"view_type_name", signature.viewType.name},
	    {"extraction_schema_version", signature.extractionSchemaVersion},
	    {"match_schema_version", signature.matchSchemaVersion}};
}

PatchSignature patchSignatureFromJson(const json& value)
{
	PatchSignature signature;
	signature.schemaVersion = value.value("schema_version", uint32_t {1});
	signature.family = value.at("family").get<FunctionFamilyId>();
	signature.vulnerableBinaryIdentity = value.value("vulnerable_binary_identity", "");
	signature.fixedBinaryIdentity = value.value("fixed_binary_identity", "");
	signature.vulnerable = occurrenceFromJson(value.at("vulnerable"));
	signature.fixed = occurrenceFromJson(value.at("fixed"));
	signature.wholeFunctionSimilarity = value.value("whole_function_similarity", 0.0);
	signature.confidence = value.value("confidence", 0.0);
	signature.vulnerablePatchBlocks =
	    value.value("vulnerable_patch_blocks", std::vector<uint64_t> {});
	signature.fixedPatchBlocks = value.value("fixed_patch_blocks", std::vector<uint64_t> {});
	signature.vulnerablePatchBlockIndices =
	    value.value("vulnerable_patch_block_indices", std::vector<size_t> {});
	signature.fixedPatchBlockIndices =
	    value.value("fixed_patch_block_indices", std::vector<size_t> {});
	signature.pairedChangedBlocks = value.value("paired_changed_blocks", size_t {0});
	signature.removedBlocks = value.value("removed_blocks", size_t {0});
	signature.addedBlocks = value.value("added_blocks", size_t {0});
	signature.changedSemanticLines = value.value("changed_semantic_lines", size_t {0});
	signature.changedTokenSpans = value.value("changed_token_spans", size_t {0});
	for (const auto& line : value.value("changed_lines", json::array()))
	{
		auto address = [&line](const char* key) -> std::optional<uint64_t> {
			auto found = line.find(key);
			return found != line.end() && found->is_number_unsigned()
			    ? std::optional<uint64_t>(found->get<uint64_t>()) : std::nullopt;
		};
		signature.changedLines.push_back({line.value("operation", ""),
		    line.value("vulnerable_text", ""), line.value("fixed_text", ""),
		    address("vulnerable_address"), address("fixed_address")});
	}
	signature.changedTokens =
	    value.value("changed_tokens", std::vector<std::string> {});
	signature.changedCallTargets =
	    value.value("changed_call_targets", std::vector<std::string> {});
	signature.changedConstants =
	    value.value("changed_constants", std::vector<std::string> {});
	signature.changedStringReferences =
	    value.value("changed_string_references", std::vector<std::string> {});
	if (auto relevance = value.find("security_relevance");
	    relevance != value.end() && relevance->is_object())
	{
		signature.securityRelevance.state = static_cast<SecurityRelevanceState>(
		    relevance->value("state", 0));
		signature.securityRelevance.score = relevance->value("score", 0.0);
		signature.securityRelevance.evidence =
		    relevance->value("evidence", std::vector<std::string> {});
		signature.securityRelevance.counterEvidence =
		    relevance->value("counter_evidence", std::vector<std::string> {});
	}
	int viewType = value.value("view_type", static_cast<int>(NormalFunctionGraph));
	std::string viewTypeName = value.value("view_type_name", "");
	signature.viewType = viewType == static_cast<int>(HighLevelLanguageRepresentationFunctionGraph)
	    ? BinaryNinja::FunctionViewType(viewTypeName)
	    : BinaryNinja::FunctionViewType(static_cast<BNFunctionGraphType>(viewType));
	signature.extractionSchemaVersion = value.value("extraction_schema_version", 0);
	signature.matchSchemaVersion = value.value("match_schema_version", 0);
	return signature;
}

json classificationJson(const PatchClassification& classification)
{
	return {{"binary", classification.binary},
	    {"occurrence", occurrenceJson(classification.occurrence)},
	    {"state", static_cast<int>(classification.state)},
	    {"state_name", patchStateName(classification.state)},
	    {"vulnerable_similarity", classification.vulnerableSimilarity},
	    {"fixed_similarity", classification.fixedSimilarity},
	    {"score_margin", classification.scoreMargin},
	    {"patch_region_coverage", classification.patchRegionCoverage},
	    {"evidence", classification.evidence}};
}

PatchClassification classificationFromJson(const json& value)
{
	PatchClassification classification;
	classification.binary = value.at("binary").get<CorpusBinaryId>();
	classification.occurrence = occurrenceFromJson(value.at("occurrence"));
	classification.state = static_cast<PatchState>(value.value("state", 5));
	classification.vulnerableSimilarity = value.value("vulnerable_similarity", 0.0);
	classification.fixedSimilarity = value.value("fixed_similarity", 0.0);
	classification.scoreMargin = value.value("score_margin", 0.0);
	classification.patchRegionCoverage = value.value("patch_region_coverage", 0.0);
	classification.evidence = value.value("evidence", std::vector<std::string> {});
	return classification;
}

json manifestPayload(const CorpusManifest& manifest)
{
	json binaries = json::array();
	for (const auto& binary : manifest.binaries)
		binaries.push_back({{"id", binary.id}, {"display_name", binary.displayName},
		    {"persistent_identity", binary.persistentIdentity},
		    {"function_count", binary.functionCount},
		    {"instruction_count", binary.instructionCount}, {"dirty", binary.dirty}});
	json edges = json::array();
	for (const auto& edge : manifest.plan.edges)
	{
		json item = edgeJson(edge);
		auto state = manifest.pairStates.find(edge);
		item["state"] = state == manifest.pairStates.end()
		    ? static_cast<int>(CorpusPairState::Queued) : static_cast<int>(state->second);
		auto error = manifest.pairErrors.find(edge);
		if (error != manifest.pairErrors.end() && !error->second.empty())
			item["error"] = error->second;
		edges.push_back(std::move(item));
	}
	json payload = {{"schema_version", manifest.schemaVersion},
	    {"binaries", std::move(binaries)}, {"mode", static_cast<int>(manifest.plan.mode)},
	    {"binary_order", manifest.plan.binaryOrder},
	    {"reference_overlay", manifest.plan.referenceOverlay}, {"edges", std::move(edges)},
	    {"partial", manifest.partial}, {"metric_filters", filtersJson(manifest.metricFilters)},
	    {"patch_settings", {{"minimum_score_margin", manifest.patchMinimumScoreMargin},
	        {"minimum_coverage", manifest.patchMinimumCoverage}}},
	    {"run_telemetry", {{"elapsed_us", manifest.lastRunElapsedMicroseconds},
	        {"starting_resident_bytes", manifest.lastRunStartingResidentBytes},
	        {"peak_resident_bytes", manifest.lastRunPeakResidentBytes}}},
	    {"family_settings", {{"minimum_similarity", manifest.familySettings.minimumSimilarity},
	        {"minimum_confidence", manifest.familySettings.minimumConfidence},
	        {"conflict_threshold", manifest.familySettings.conflictThreshold},
	        {"require_mutual_best_heuristics",
	            manifest.familySettings.requireMutualBestHeuristics}}}};
	if (manifest.plan.reference)
		payload["reference"] = *manifest.plan.reference;
	if (manifest.patchSignature)
		payload["patch_signature"] = patchSignatureJson(*manifest.patchSignature);
	json classifications = json::array();
	for (const auto& [binary, classification] : manifest.patchClassifications)
	{
		(void)binary;
		classifications.push_back(classificationJson(classification));
	}
	payload["patch_classifications"] = std::move(classifications);
	return payload;
}

CorpusManifest manifestFromPayload(const json& payload)
{
	CorpusManifest manifest;
	manifest.schemaVersion = payload.at("schema_version").get<uint32_t>();
	if (manifest.schemaVersion != 1)
		throw std::runtime_error("unsupported corpus manifest schema");
	for (const auto& value : payload.at("binaries"))
		manifest.binaries.push_back({value.at("id").get<CorpusBinaryId>(),
		    value.value("display_name", ""), value.value("persistent_identity", ""),
		    value.value("function_count", size_t {0}),
		    value.value("instruction_count", size_t {0}), value.value("dirty", false)});
	manifest.plan.mode = static_cast<CorpusMode>(payload.at("mode").get<int>());
	manifest.plan.binaryOrder = payload.at("binary_order").get<std::vector<CorpusBinaryId>>();
	if (payload.contains("reference"))
		manifest.plan.reference = payload.at("reference").get<CorpusBinaryId>();
	manifest.plan.referenceOverlay = payload.value("reference_overlay", false);
	for (const auto& value : payload.at("edges"))
	{
		CorpusEdgeKey edge = edgeFromJson(value);
		manifest.plan.edges.push_back(edge);
		manifest.pairStates[edge] = static_cast<CorpusPairState>(
		    value.value("state", static_cast<int>(CorpusPairState::Queued)));
		manifest.pairErrors[edge] = value.value("error", "");
	}
	manifest.plan.valid = true;
	manifest.partial = payload.value("partial", false);
	manifest.metricFilters = filtersFromJson(payload.value("metric_filters", json::object()));
	const json& patchSettings = payload.value("patch_settings", json::object());
	manifest.patchMinimumScoreMargin = patchSettings.value("minimum_score_margin", 0.1);
	manifest.patchMinimumCoverage = patchSettings.value("minimum_coverage", 0.6);
	const json& telemetry = payload.value("run_telemetry", json::object());
	manifest.lastRunElapsedMicroseconds = telemetry.value("elapsed_us", uint64_t {0});
	manifest.lastRunStartingResidentBytes =
	    telemetry.value("starting_resident_bytes", uint64_t {0});
	manifest.lastRunPeakResidentBytes =
	    telemetry.value("peak_resident_bytes", uint64_t {0});
	const json& settings = payload.value("family_settings", json::object());
	manifest.familySettings.minimumSimilarity = settings.value("minimum_similarity", 0.0);
	manifest.familySettings.minimumConfidence = settings.value("minimum_confidence", 0.0);
	manifest.familySettings.conflictThreshold = settings.value("conflict_threshold", 0.8);
	manifest.familySettings.requireMutualBestHeuristics =
	    settings.value("require_mutual_best_heuristics", true);
	if (payload.contains("patch_signature"))
	{
		PatchSignature signature = patchSignatureFromJson(payload.at("patch_signature"));
		if (patchSignatureCompatible(signature))
			manifest.patchSignature = std::move(signature);
	}
	if (manifest.patchSignature)
		for (const auto& value : payload.value("patch_classifications", json::array()))
		{
			PatchClassification classification = classificationFromJson(value);
			manifest.patchClassifications[classification.binary] = std::move(classification);
		}
	return manifest;
}

bool saveBytes(const std::string& path, const QByteArray& bytes, std::string* error)
{
	QFileInfo info(QString::fromStdString(path));
	if (!QDir().mkpath(info.absolutePath()))
	{
		if (error) *error = "Could not create the output directory";
		return false;
	}
	QSaveFile file(info.absoluteFilePath());
	if (!file.open(QIODevice::WriteOnly)
	    || file.write(bytes) != bytes.size() || !file.commit())
	{
		file.cancelWriting();
		if (error) *error = "Could not write the output file";
		return false;
	}
	return true;
}

json metricsJson(const CorpusPairMetrics& metrics)
{
	return {{"source_function_count", metrics.sourceFunctionCount},
	    {"target_function_count", metrics.targetFunctionCount},
	    {"source_instruction_count", metrics.sourceInstructionCount},
	    {"target_instruction_count", metrics.targetInstructionCount},
	    {"accepted_match_count", metrics.acceptedMatchCount},
	    {"exact_match_count", metrics.exactMatchCount},
	    {"changed_match_count", metrics.changedMatchCount},
	    {"source_coverage", metrics.sourceCoverage},
	    {"target_coverage", metrics.targetCoverage},
	    {"instruction_coverage_source", metrics.instructionCoverageSource},
	    {"instruction_coverage_target", metrics.instructionCoverageTarget},
	    {"mean_similarity", metrics.meanSimilarity},
	    {"median_similarity", metrics.medianSimilarity},
	    {"mean_confidence", metrics.meanConfidence},
	    {"matches_by_method", metrics.matchesByMethod}};
}

json linkJson(const FamilyLinkEvidence& link)
{
	return {{"source", occurrenceJson(link.source)}, {"target", occurrenceJson(link.target)},
	    {"similarity", link.similarity}, {"confidence", link.confidence},
	    {"match_type", link.matchType}, {"origin", edgeJson(link.origin)},
	    {"content_match", link.contentMatch}};
}

std::string csvField(const std::string& value)
{
	std::string escaped = value;
	// Spreadsheet applications may evaluate even quoted CSV fields as formulas.
	// Prefix untrusted text before quoting, including whitespace-prefixed formulas.
	const size_t first = escaped.find_first_not_of(" \t\r\n");
	if ((!escaped.empty() && (escaped.front() == '\t' || escaped.front() == '\r'
	        || escaped.front() == '\n'))
	    || (first != std::string::npos && std::string("=+-@").find(escaped[first])
	        != std::string::npos))
		escaped.insert(escaped.begin(), '\'');
	size_t position = 0;
	while ((position = escaped.find('"', position)) != std::string::npos)
	{
		escaped.insert(position, 1, '"');
		position += 2;
	}
	return '"' + escaped + '"';
}

}  // namespace

std::optional<std::string> corpusManifestPath(const CorpusManifest& manifest)
{
	if (manifest.binaries.empty()
	    || std::any_of(manifest.binaries.begin(), manifest.binaries.end(),
	        [](const CorpusBinary& binary) {
		        return binary.dirty || binary.persistentIdentity.empty();
	        }))
		return std::nullopt;
	std::string material = "binja-diff-corpus\n1\n"
	    + std::to_string(static_cast<int>(manifest.plan.mode)) + "\n"
	    + (manifest.plan.reference ? std::to_string(*manifest.plan.reference) : "none") + "\n"
	    + (manifest.plan.referenceOverlay ? "overlay" : "no-overlay") + "\n";
	for (const auto& binary : manifest.binaries)
		material += binary.persistentIdentity + "\n";
	QByteArray digest = QCryptographicHash::hash(
	    QByteArray::fromStdString(material), QCryptographicHash::Sha256).toHex();
	QString root = diskCacheRoot();
	if (root.isEmpty())
		return std::nullopt;
	return QDir(root).filePath("binja_diff_view/corpora/" + digest + ".json").toStdString();
}

bool saveCorpusManifest(const std::string& path, const CorpusManifest& manifest,
	std::string* error)
{
	QByteArray payload = QByteArray::fromStdString(manifestPayload(manifest).dump());
	json envelope = {{"magic", kManifestMagic},
	    {"sha256", QCryptographicHash::hash(payload, QCryptographicHash::Sha256)
	        .toHex().toStdString()}, {"payload", json::parse(payload.constData())}};
	return saveBytes(path, QByteArray::fromStdString(envelope.dump(2)), error);
}

std::optional<CorpusManifest> loadCorpusManifest(const std::string& path,
	std::string* error)
{
	QFile file(QString::fromStdString(path));
	if (!file.open(QIODevice::ReadOnly))
		return std::nullopt;
	try
	{
		json envelope = json::parse(file.readAll().constData());
		if (envelope.value("magic", "") != kManifestMagic || !envelope.contains("payload"))
			throw std::runtime_error("invalid corpus manifest envelope");
		QByteArray payload = QByteArray::fromStdString(envelope.at("payload").dump());
		std::string digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256)
		    .toHex().toStdString();
		if (digest != envelope.value("sha256", ""))
			throw std::runtime_error("corpus manifest integrity check failed");
		return manifestFromPayload(envelope.at("payload"));
	}
	catch (const std::exception& exception)
	{
		if (error) *error = exception.what();
		return std::nullopt;
	}
}

bool exportCorpusJson(const std::string& path, const CorpusManifest& manifest,
	const std::map<CorpusEdgeKey, CorpusPairResultRef>& pairs,
	const std::vector<FunctionFamily>& families, std::string* error,
	const std::function<bool()>& cancelled)
{
	return writeExport(path, error, cancelled, [&](std::ostream& output) {
		json root = manifestPayload(manifest);
		output << "{\n";
		for (auto field = root.begin(); field != root.end(); ++field)
			output << json(field.key()) << ": " << std::setw(2) << field.value() << ",\n";
		output << "\"pair_results\": [";
		bool first = true;
		for (const auto& [edge, pair] : pairs)
		{
			json value = edgeJson(edge);
			value["state"] = pair ? static_cast<int>(pair->state)
			    : static_cast<int>(CorpusPairState::Queued);
			if (pair)
			{
				value["error"] = pair->error;
				value["elapsed_ms"] = pair->elapsedMilliseconds;
				value["elapsed_us"] = pair->elapsedMicroseconds;
				value["raw_metrics"] = metricsJson(pair->rawMetrics);
				value["filtered_metrics"] = metricsJson(pair->metrics);
			}
			if (!first) output << ",";
			first = false;
			output << "\n" << std::setw(2) << value;
		}
		output << "\n],\n\"families\": [";
		first = true;
		for (const auto& family : families)
		{
			if (cancelled && cancelled()) throw std::runtime_error("Export cancelled");
			json members = json::array();
			for (const auto& [binary, occurrence] : family.members)
			{
				(void)binary;
				members.push_back(occurrenceJson(occurrence));
			}
			json accepted = json::array();
			for (const auto& link : family.acceptedLinks) accepted.push_back(linkJson(link));
			json rejected = json::array();
			for (const auto& link : family.rejectedLinks) rejected.push_back(linkJson(link));
			if (!first) output << ",";
			first = false;
			json value = {{"id", family.id},
			    {"state", functionFamilyStateName(family.state)}, {"members", std::move(members)},
			    {"minimum_similarity", family.minimumSimilarity},
			    {"minimum_confidence", family.minimumConfidence},
			    {"accepted_links", std::move(accepted)}, {"rejected_links", std::move(rejected)}};
			output << "\n" << std::setw(2) << value;
		}
		output << "\n]\n}";
	});
}

bool exportCorpusCsv(const std::string& path, const CorpusManifest& manifest,
	const std::map<CorpusEdgeKey, CorpusPairResultRef>& pairs,
	const std::vector<FunctionFamily>& families, std::string* error,
	const std::function<bool()>& cancelled)
{
	return writeExport(path, error, cancelled, [&](std::ostream& output) {
		constexpr size_t kColumns = 21;
		output << "record_type,source,target,source_address,target_address,source_name,target_name,"
		          "family,binary,address,name,state,similarity,confidence,match_type,metric,"
		          "raw_value,filtered_value,persistent_identity,error,evidence\n";
		auto writeRow = [&output](const std::vector<std::string>& values) {
			for (size_t index = 0; index < kColumns; index++)
			{
				if (index)
					output << ',';
				output << csvField(index < values.size() ? values[index] : std::string());
			}
			output << '\n';
		};
		auto hex = [](uint64_t value) {
			std::ostringstream stream;
			stream << "0x" << std::hex << value;
			return stream.str();
		};
		for (const auto& binary : manifest.binaries)
		{
			std::vector<std::string> row(kColumns);
			row[0] = "binary";
			row[8] = std::to_string(binary.id);
			row[10] = binary.displayName;
			row[18] = binary.persistentIdentity;
			row[20] = binary.dirty ? "dirty or transient identity" : "clean saved identity";
			writeRow(row);
		}
		auto writeMetric = [&writeRow](const CorpusEdgeKey& edge,
		    const std::string& metric, double raw, double filtered) {
			std::vector<std::string> row(kColumns);
			row[0] = "pair_metric";
			row[1] = std::to_string(edge.source);
			row[2] = std::to_string(edge.target);
			row[15] = metric;
			row[16] = std::to_string(raw);
			row[17] = std::to_string(filtered);
			writeRow(row);
		};
		for (const auto& [edge, pair] : pairs)
		{
			std::vector<std::string> row(kColumns);
			row[0] = "pair";
			row[1] = std::to_string(edge.source);
			row[2] = std::to_string(edge.target);
			row[11] = std::to_string(pair ? static_cast<int>(pair->state) : 0);
			row[19] = pair ? pair->error : std::string();
			writeRow(row);
			if (!pair)
				continue;
			const CorpusPairMetrics& raw = pair->rawMetrics;
			const CorpusPairMetrics& filtered = pair->metrics;
			writeMetric(edge, "source_function_count", raw.sourceFunctionCount,
			    filtered.sourceFunctionCount);
			writeMetric(edge, "target_function_count", raw.targetFunctionCount,
			    filtered.targetFunctionCount);
			writeMetric(edge, "source_instruction_count", raw.sourceInstructionCount,
			    filtered.sourceInstructionCount);
			writeMetric(edge, "target_instruction_count", raw.targetInstructionCount,
			    filtered.targetInstructionCount);
			writeMetric(edge, "accepted_match_count", raw.acceptedMatchCount,
			    filtered.acceptedMatchCount);
			writeMetric(edge, "exact_match_count", raw.exactMatchCount,
			    filtered.exactMatchCount);
			writeMetric(edge, "changed_match_count", raw.changedMatchCount,
			    filtered.changedMatchCount);
			writeMetric(edge, "source_coverage", raw.sourceCoverage, filtered.sourceCoverage);
			writeMetric(edge, "target_coverage", raw.targetCoverage, filtered.targetCoverage);
			writeMetric(edge, "instruction_coverage_source", raw.instructionCoverageSource,
			    filtered.instructionCoverageSource);
			writeMetric(edge, "instruction_coverage_target", raw.instructionCoverageTarget,
			    filtered.instructionCoverageTarget);
			writeMetric(edge, "mean_similarity", raw.meanSimilarity, filtered.meanSimilarity);
			writeMetric(edge, "median_similarity", raw.medianSimilarity,
			    filtered.medianSimilarity);
			writeMetric(edge, "mean_confidence", raw.meanConfidence, filtered.meanConfidence);
			writeMetric(edge, "source_only_instruction_count", raw.sourceOnlyInstructionCount,
			    filtered.sourceOnlyInstructionCount);
			writeMetric(edge, "target_only_instruction_count", raw.targetOnlyInstructionCount,
			    filtered.targetOnlyInstructionCount);
			std::set<std::string> methods;
			for (const auto& [method, count] : raw.matchesByMethod)
			{
				(void)count;
				methods.insert(method);
			}
			for (const auto& [method, count] : filtered.matchesByMethod)
			{
				(void)count;
				methods.insert(method);
			}
			for (const auto& method : methods)
				writeMetric(edge, "matches_by_method:" + method,
				    raw.matchesByMethod.count(method) ? raw.matchesByMethod.at(method) : 0,
				    filtered.matchesByMethod.count(method)
				        ? filtered.matchesByMethod.at(method) : 0);
		}
		for (const auto& family : families)
		{
			for (const auto& [binary, occurrence] : family.members)
			{
				std::vector<std::string> row(kColumns);
				row[0] = "occurrence";
				row[7] = std::to_string(family.id);
				row[8] = std::to_string(binary);
				row[9] = hex(occurrence.address);
				row[10] = occurrence.name;
				row[11] = functionFamilyStateName(family.state);
				writeRow(row);
			}
			for (const auto& link : family.acceptedLinks)
			{
				std::vector<std::string> row(kColumns);
				row[0] = "accepted_link";
				row[1] = std::to_string(link.origin.source);
				row[2] = std::to_string(link.origin.target);
				row[3] = hex(link.source.address);
				row[4] = hex(link.target.address);
				row[5] = link.source.name;
				row[6] = link.target.name;
				row[7] = std::to_string(family.id);
				row[12] = std::to_string(link.similarity);
				row[13] = std::to_string(link.confidence);
				row[14] = link.matchType;
				writeRow(row);
			}
			for (const auto& link : family.rejectedLinks)
			{
				std::vector<std::string> row(kColumns);
				row[0] = "rejected_link";
				row[1] = std::to_string(link.origin.source);
				row[2] = std::to_string(link.origin.target);
				row[3] = hex(link.source.address);
				row[4] = hex(link.target.address);
				row[5] = link.source.name;
				row[6] = link.target.name;
				row[7] = std::to_string(family.id);
				row[12] = std::to_string(link.similarity);
				row[13] = std::to_string(link.confidence);
				row[14] = link.matchType;
				writeRow(row);
			}
		}
		if (manifest.patchSignature)
		{
			std::vector<std::string> row(kColumns);
			row[0] = "patch_signature";
			row[7] = std::to_string(manifest.patchSignature->family);
			row[18] = manifest.patchSignature->vulnerableBinaryIdentity + " | "
			    + manifest.patchSignature->fixedBinaryIdentity;
			row[20] = patchSignatureJson(*manifest.patchSignature).dump();
			writeRow(row);
		}
		for (const auto& [binary, classification] : manifest.patchClassifications)
		{
			std::vector<std::string> row(kColumns);
			row[0] = "patch_classification";
			row[7] = std::to_string(manifest.patchSignature
			    ? manifest.patchSignature->family : 0);
			row[8] = std::to_string(binary);
			row[9] = hex(classification.occurrence.address);
			row[10] = classification.occurrence.name;
			row[11] = patchStateName(classification.state);
			row[12] = std::to_string(classification.vulnerableSimilarity);
			row[13] = std::to_string(classification.fixedSimilarity);
			row[15] = "score_margin_and_patch_coverage";
			row[16] = std::to_string(classification.scoreMargin);
			row[17] = std::to_string(classification.patchRegionCoverage);
			for (size_t index = 0; index < classification.evidence.size(); index++)
			{
				if (index)
					row[20] += "; ";
				row[20] += classification.evidence[index];
			}
			writeRow(row);
		}
	});
}

}  // namespace bn_graph_diff
