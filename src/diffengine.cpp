#include "diffengine.h"
#include "filedigest.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLibrary>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <atomic>
#include <exception>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <stdexcept>
#include <thread>

#if defined(Q_OS_UNIX)
#include <dlfcn.h>
#endif

#include "nlohmann/json.hpp"
#include "diffmodel.h"
#include "diskcache.h"

using namespace BinaryNinja;
using json = nlohmann::json;

namespace bn_graph_diff {

namespace {

std::string qualifiedLeafName(const std::string& name)
{
	size_t lastScope = std::string::npos;
	int angleDepth = 0;
	int parenthesisDepth = 0;
	int bracketDepth = 0;
	for (size_t i = 0; i < name.size(); i++)
	{
		switch (name[i])
		{
		case '<': angleDepth++; break;
		case '>': angleDepth = std::max(0, angleDepth - 1); break;
		case '(': parenthesisDepth++; break;
		case ')': parenthesisDepth = std::max(0, parenthesisDepth - 1); break;
		case '[': bracketDepth++; break;
		case ']': bracketDepth = std::max(0, bracketDepth - 1); break;
		case ':':
			if (i + 1 < name.size() && name[i + 1] == ':'
			    && angleDepth == 0 && parenthesisDepth == 0 && bracketDepth == 0)
			{
				lastScope = i;
				i++;
			}
			break;
		default: break;
		}
	}
	return lastScope == std::string::npos ? name : name.substr(lastScope + 2);
}

bool isMangledName(const std::string& name)
{
#if BN_CURRENT_CORE_ABI_VERSION >= 180
	return IsMSVCMangledString(name) || IsGNU3MangledString(name);
#else
	return (!name.empty() && name.front() == '?') || IsGNU3MangledString(name);
#endif
}

std::optional<std::string> demangledLeaf(BinaryViewRef view, const std::string& rawName)
{
#if BN_CURRENT_CORE_ABI_VERSION >= 180
	auto demangled = Demangler::DemangleAny(rawName,
	    view ? DemanglerConfig::ForBinaryView(view) : DemanglerConfig::Default());
	// DemangleAny needs platform context for MSVC symbols. Normal extraction
	// has a BinaryView, but retain a Windows fallback for callers that do not.
	if (!demangled && IsMSVCMangledString(rawName))
	{
		Ref<Platform> platform = view ? view->GetDefaultPlatform()
		                              : Platform::GetByName("windows-x86_64");
		if (platform)
			demangled = DemangleMS(platform, rawName, false);
	}
	if (demangled && !demangled->name.IsEmpty())
		return demangled->name.back();
#else
	QualifiedName demangled;
	if (!rawName.empty() && rawName.front() == '?')
	{
		Ref<Architecture> architecture = Architecture::GetByName("x86_64");
		Ref<Type> type;
		bool ok = architecture
		    && (view ? DemangleMS(architecture, rawName, type, demangled, view.GetPtr())
		             : DemangleMS(architecture, rawName, type, demangled, false));
		if (ok && !demangled.IsEmpty())
			return demangled.back();
	}
	if (DemangleLLVM(rawName, demangled, view ? view.GetPtr() : nullptr)
	    && !demangled.IsEmpty())
		return demangled.back();
#endif
	return std::nullopt;
}

}  // namespace

std::string functionMatchName(BinaryViewRef view, Ref<Symbol> symbol)
{
	if (!symbol)
		return {};
	const std::string shortName = symbol->GetShortName();
	const std::string rawName = symbol->GetRawName();
	if (!shortName.empty() && (rawName.empty() || shortName != rawName))
		return shortName;
	if (!rawName.empty())
		if (auto demangled = demangledLeaf(view, rawName))
			return *demangled;
	if (!shortName.empty())
		return shortName;
	return symbol->GetFullName();
}

std::string functionDisplayName(BinaryViewRef view, Ref<Symbol> symbol)
{
	if (!symbol)
		return {};
	const std::string shortName = symbol->GetShortName();
	if (!shortName.empty() && shortName.find("::") == std::string::npos
	    && !isMangledName(shortName))
		return shortName;
	const std::string rawName = symbol->GetRawName();
	if (!rawName.empty())
		if (auto demangled = demangledLeaf(view, rawName))
			return *demangled;

	const std::string matchName = functionMatchName(view, symbol);
	return qualifiedLeafName(matchName);
}

namespace {

constexpr const char* kExtractionCacheMagic = "binja-diff-extraction-cache-v1";

// Conservative retained-allocation estimate, including map nodes and evidence
// strings. This is not a measurement of allocator overhead or process RSS.
size_t estimatedMatchBytes(const FunctionMatchMaps& maps)
{
	auto strings = [](const std::vector<std::string>& values) {
		size_t bytes = values.capacity() * sizeof(std::string);
		for (const auto& value : values) bytes += value.capacity() + 1;
		return bytes;
	};
	auto summary = [&](const FunctionSummary& value) {
		return value.name.capacity() + value.securityState.capacity()
		    + value.suggestedCounterpartName.capacity() + 3
		    + strings(value.securityEvidence) + strings(value.securityCounterEvidence);
	};
	size_t bytes = sizeof(maps) + maps.error.capacity() + 1
	    + maps.matches.capacity() * sizeof(FunctionMatchRecord)
	    + (maps.unmatchedA.capacity() + maps.unmatchedB.capacity()) * sizeof(FunctionSummary);
	for (const auto& match : maps.matches)
		bytes += summary(match.a) + summary(match.b) + match.matchType.capacity()
		    + match.contentMatch.capacity() + match.securityState.capacity() + 3
		    + strings(match.securityEvidence) + strings(match.securityCounterEvidence);
	for (const auto& value : maps.unmatchedA) bytes += summary(value);
	for (const auto& value : maps.unmatchedB) bytes += summary(value);
	for (const auto* map : {&maps.renameMap, &maps.addrMap})
		for (const auto& entry : *map)
			bytes += sizeof(entry) + 4 * sizeof(void*)
			    + entry.first.capacity() + entry.second.capacity() + 2;
	bytes += maps.funcMap.size() * (sizeof(decltype(maps.funcMap)::value_type) + 4 * sizeof(void*));
	return bytes;
}

// Some Binary Ninja disassembly renderers expose an IntegerToken's semantic
// value as zero or as an already-adjusted value while retaining the actual
// displacement in its text. Parse the displayed non-negative literal as a
// fallback so image-base-relative table offsets receive the same treatment as
// ordinary section-backed address tokens.
std::optional<uint64_t> displayedIntegerValue(std::string_view text)
{
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
		text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
		text.remove_suffix(1);
	if (!text.empty() && text.front() == '+')
		text.remove_prefix(1);
	if (text.empty() || text.front() == '-')
		return std::nullopt;

	int base = 10;
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
	{
		base = 16;
		text.remove_prefix(2);
	}
	if (text.empty())
		return std::nullopt;
	uint64_t value = 0;
	auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
	if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
		return std::nullopt;
	return value;
}

bool addressResolvesIntoImage(BinaryView* bv, uint64_t value)
{
	if (bv->GetSegmentAt(value) || !bv->GetSectionsAt(value).empty())
		return true;
	return value <= UINT64_MAX - bv->GetStart()
	    && (bv->GetSegmentAt(bv->GetStart() + value)
	        || !bv->GetSectionsAt(bv->GetStart() + value).empty());
}

// AArch64 builds an address from a page base and a low immediate, as in
// `adrp x3, page` followed by `add x3, x3, #0x97`. Analysis usually records the
// resolved address as a code reference from the second instruction; when the
// register is consumed later instead, its value after the instruction is that
// constant address. An immediate equal to the address's page offset is address
// material: moving a string or global changes it without changing behavior.
// Constants that do not form an absolute image address, including bounds and
// masks, remain literal. Other architectures have no page-offset immediates, so
// `mov eax, 1` and `mov eax, 2` stay a semantic change.
static bool isResolvedPageOffset(BinaryView* bv, const ReferenceSource& source,
    const std::vector<InstructionTextToken>& lineTokens, const InstructionTextToken& token)
{
	constexpr uint64_t kPageOffsetMask = 0xfff;
	if (token.type != IntegerToken || !source.arch
	    || source.arch->GetName().rfind("aarch64", 0) != 0)
		return false;
	std::optional<uint64_t> value = token.value;
	if (*value > kPageOffsetMask)
		value = displayedIntegerValue(token.text);
	if (!value || *value > kPageOffsetMask)
		return false;
	// The target must itself be mapped. The image-relative fallback of
	// addressResolvesIntoImage would accept a small constant as base + value,
	// which trivially shares its own page offset.
	auto isPageOffsetOf = [&](uint64_t target) {
		return target > kPageOffsetMask && (target & kPageOffsetMask) == *value
		    && (bv->GetSegmentAt(target) || !bv->GetSectionsAt(target).empty());
	};
	for (uint64_t target : bv->GetCodeReferencesFrom(source))
		if (isPageOffsetOf(target))
			return true;
	auto destination = std::find_if(lineTokens.begin(), lineTokens.end(),
	    [](const InstructionTextToken& candidate) { return candidate.type == RegisterToken; });
	if (destination == lineTokens.end() || !source.func)
		return false;
	RegisterValue result = source.func->GetRegisterValueAfterInstruction(
	    source.arch, source.addr, source.arch->GetRegisterByName(destination->text));
	return result.IsConstant() && isPageOffsetOf(static_cast<uint64_t>(result.value));
}

// A database restores functions lazily. Until something requests a function's
// IL, its disassembly omits call-target symbols, data symbols, and analysis
// annotations, so identical code could be extracted differently depending on
// which functions had been viewed. The caller renders while holding the
// returned LLIL: the core frees unreferenced IL, and a later request would
// generate it again.
thread_local bool t_materializingFunctionAnalysis = false;

static Ref<LowLevelILFunction> materializeFunctionAnalysis(Function* func)
{
	if (Ref<LowLevelILFunction> il = func->GetLowLevelILIfAvailable())
		return il;
	t_materializingFunctionAnalysis = true;
	Ref<LowLevelILFunction> il = func->GetLowLevelIL();
	t_materializingFunctionAnalysis = false;
	return il;
}

bool analysisOnlyMatchToken(const InstructionTextToken& token, bool annotationSeen)
{
	// These token kinds are added by analysis/display and are not emitted by
	// architecture instruction renderers. They must not become machine-content
	// evidence even when the renderer places them between real operand tokens.
	// ValueLocationToken has the stable core ABI value 40, but older supported
	// Binary Ninja SDKs do not expose its C++ enum name.
	if (static_cast<uint32_t>(token.type) == 40)
		return true;
	switch (token.type)
	{
	case AnnotationToken:
	case CommentToken:
	case TagToken:
	case LocalVariableToken:
	case StackVariableToken:
	case ArgumentNameToken:
	case PossibleValueToken:
	case PossibleValueTypeToken:
	case StringToken:
	case CollapseStateIndicatorToken:
	case CollapsedInformationToken:
		return true;
	default: break;
	}
	// An annotation may be followed by punctuation or a value preview with a
	// generic token kind. Real architecture operands retain their operand index,
	// which also lets extraction resume after an inline annotation.
	return annotationSeen
	    && token.operand == static_cast<size_t>(BN_INVALID_OPERAND);
}

json functionSummaryToJson(const FunctionSummary& function)
{
	json value = {{"name", function.name}, {"address", function.address},
	    {"size", function.size}, {"instruction_count", function.instructionCount},
	    {"security_state", function.securityState}, {"security_score", function.securityScore},
	    {"security_direct_score", function.securityDirectScore},
	    {"security_evidence", function.securityEvidence},
	    {"security_counter_evidence", function.securityCounterEvidence},
	    {"security_propagated", function.securityPropagated}};
	if (function.suggestedCounterpartAddress)
	{
		value["suggested_counterpart_address"] = function.suggestedCounterpartAddress;
		value["suggested_counterpart_name"] = function.suggestedCounterpartName;
		value["suggested_body_similarity"] = function.suggestedBodySimilarity;
	}
	return value;
}

FunctionSummary functionSummaryFromJson(const json& value)
{
	if (!value.is_object())
		throw std::runtime_error("function summary is not an object");
	FunctionSummary function;
	function.name = value.value("name", "");
	function.address = value.value("address", uint64_t {0});
	function.size = value.value("size", uint64_t {0});
	function.instructionCount = value.value("instruction_count", size_t {0});
	function.suggestedCounterpartAddress =
	    value.value("suggested_counterpart_address", uint64_t {0});
	function.suggestedCounterpartName = value.value("suggested_counterpart_name", "");
	function.suggestedBodySimilarity = value.value("suggested_body_similarity", 0.0);
	function.securityState = value.value("security_state", "Unknown");
	function.securityScore = value.value("security_score", 0.0);
	function.securityDirectScore = value.value("security_direct_score", 0.0);
	function.securityEvidence = value.value("security_evidence", std::vector<std::string> {});
	function.securityCounterEvidence =
	    value.value("security_counter_evidence", std::vector<std::string> {});
	function.securityPropagated = value.value("security_propagated", false);
	return function;
}

template <typename Record>
void securityAssessmentFromJson(const json& value, Record& record)
{
	if (!value.is_object())
		return;
	record.securityState = value.value("state", "Unknown");
	record.securityScore = value.value("score", 0.0);
	record.securityDirectScore = value.value("direct_score", 0.0);
	record.securityEvidence = value.value("evidence", std::vector<std::string> {});
	record.securityCounterEvidence =
	    value.value("counter_evidence", std::vector<std::string> {});
	record.securityPropagated = value.value("propagated", false);
}

json functionMatchMapsToJson(const FunctionMatchMaps& maps)
{
	json matches = json::array();
	for (const auto& match : maps.matches)
	{
		matches.push_back({{"a", functionSummaryToJson(match.a)},
		    {"b", functionSummaryToJson(match.b)}, {"similarity", match.similarity},
		    {"confidence", match.confidence}, {"match_type", match.matchType},
		    {"ambiguity_margin", match.ambiguityMargin},
		    {"content_match", match.contentMatch},
		    {"cfg_similarity", match.cfgSimilarity},
		    {"basic_block_similarity", match.basicBlockSimilarity},
		    {"instruction_similarity", match.instructionSimilarity},
		    {"edge_similarity", match.edgeSimilarity},
		    {"name_similarity", match.nameSimilarity},
		    {"call_similarity", match.callSimilarity},
		    {"neighbor_support", match.neighborSupport},
		    {"neighbor_contradictions", match.neighborContradictions},
		    {"binding_anchor", match.bindingAnchor},
		    {"security_state", match.securityState}, {"security_score", match.securityScore},
		    {"security_direct_score", match.securityDirectScore},
		    {"security_evidence", match.securityEvidence},
		    {"security_counter_evidence", match.securityCounterEvidence},
		    {"security_propagated", match.securityPropagated}});
	}
	json unmatchedA = json::array();
	for (const auto& function : maps.unmatchedA)
		unmatchedA.push_back(functionSummaryToJson(function));
	json unmatchedB = json::array();
	for (const auto& function : maps.unmatchedB)
		unmatchedB.push_back(functionSummaryToJson(function));
	return {{"matches", std::move(matches)}, {"unmatched_a", std::move(unmatchedA)},
	    {"unmatched_b", std::move(unmatchedB)}, {"similarity", maps.similarity},
	    {"matched_similarity", maps.matchedSimilarity}, {"coverage", maps.coverage},
	    {"analysis_time", maps.analysisTime}};
}

FunctionMatchMaps functionMatchMapsFromJson(const json& value)
{
	if (!value.is_object() || !value.contains("matches") || !value["matches"].is_array()
	    || !value.contains("unmatched_a") || !value["unmatched_a"].is_array()
	    || !value.contains("unmatched_b") || !value["unmatched_b"].is_array())
		throw std::runtime_error("persistent match cache has an invalid shape");
	FunctionMatchMaps maps;
	for (const auto& item : value["matches"])
	{
		if (!item.is_object() || !item.contains("a") || !item.contains("b"))
			throw std::runtime_error("persistent match cache contains an invalid match");
		FunctionMatchRecord match;
		match.a = functionSummaryFromJson(item["a"]);
		match.b = functionSummaryFromJson(item["b"]);
		match.similarity = item.value("similarity", 0.0);
		match.confidence = item.value("confidence", 0.0);
		match.ambiguityMargin = item.value("ambiguity_margin", 1.0);
		match.matchType = item.value("match_type", "Unknown");
		match.contentMatch = item.value("content_match", "Changed");
		match.cfgSimilarity = item.value("cfg_similarity", 0.0);
		match.basicBlockSimilarity = item.value("basic_block_similarity", 0.0);
		match.instructionSimilarity = item.value("instruction_similarity", 0.0);
		match.edgeSimilarity = item.value("edge_similarity", 0.0);
		match.nameSimilarity = item.value("name_similarity", 0.0);
		match.callSimilarity = item.value("call_similarity", 0.0);
		match.neighborSupport = item.value("neighbor_support", size_t(0));
		match.neighborContradictions = item.value("neighbor_contradictions", size_t(0));
		match.bindingAnchor = item.value("binding_anchor", false);
		match.securityState = item.value("security_state", "Unknown");
		match.securityScore = item.value("security_score", 0.0);
		match.securityDirectScore = item.value("security_direct_score", 0.0);
		match.securityEvidence = item.value("security_evidence", std::vector<std::string> {});
		match.securityCounterEvidence =
		    item.value("security_counter_evidence", std::vector<std::string> {});
		match.securityPropagated = item.value("security_propagated", false);
		maps.funcMap[match.a.address] = match.b.address;
		if (match.a.address != match.b.address)
		{
			char addressA[32], addressB[32];
			snprintf(addressA, sizeof(addressA), "0x%llx",
			    static_cast<unsigned long long>(match.a.address));
			snprintf(addressB, sizeof(addressB), "0x%llx",
			    static_cast<unsigned long long>(match.b.address));
			maps.addrMap[addressA] = addressB;
		}
		maps.matches.push_back(std::move(match));
	}
	for (const auto& function : value["unmatched_a"])
		maps.unmatchedA.push_back(functionSummaryFromJson(function));
	for (const auto& function : value["unmatched_b"])
		maps.unmatchedB.push_back(functionSummaryFromJson(function));
	rebuildCalleeRenameMap(maps);
	maps.matchCount = maps.matches.size();
	maps.similarity = value.value("similarity", 0.0);
	maps.matchedSimilarity = value.value("matched_similarity", 0.0);
	maps.coverage = value.value("coverage", 0.0);
	maps.analysisTime = value.value("analysis_time", 0.0);
	maps.valid = true;
	return maps;
}

}  // namespace

bool materializingFunctionAnalysis()
{
	return t_materializingFunctionAnalysis;
}

class DiffEngine::LibraryHolder
{
  public:
	QLibrary library;
};

static std::string engineLibraryName()
{
#if defined(Q_OS_WIN)
	return "rust_diff.dll";
#elif defined(Q_OS_MACOS)
	return "librust_diff.dylib";
#else
	return "librust_diff.so";
#endif
}

// Map BNInstructionTextTokenType onto the loose kind names the engine's
// canonicalizer understands (see rust_diff/src/il.rs). Unknown kinds are
// treated as literal text by the engine, which is the right default for
// keywords, operators and punctuation.
static const char* tokenKind(BNInstructionTextTokenType type)
{
	switch (type)
	{
	case LocalVariableToken:
		return "localVariable";
	case StackVariableToken:
		return "stackVariable";
	case ArgumentNameToken:
		return "localVariable";
	case IntegerToken:
		return "integer";
	case FloatingPointToken:
		return "floatingPoint";
	case PossibleValueToken:
		return "possibleValue";
	// Goto-label numbering is graph-local and renumbers freely across builds.
	case GotoLabelToken:
		return "gotoLabel";
	case PossibleAddressToken:
		return "possibleAddress";
	case AddressDisplayToken:
		return "addressDisplay";
	case CodeRelativeAddressToken:
		return "codeRelativeAddress";
	case CodeSymbolToken:
		return "codeSymbol";
	case DataSymbolToken:
		return "dataSymbol";
	case ImportToken:
		return "import";
	case IndirectImportToken:
		return "indirectImport";
	case ExternalSymbolToken:
		return "externalSymbol";
	case RegisterToken:
		return "register";
	default:
		return "text";
	}
}

DiffEngine& DiffEngine::instance()
{
	static DiffEngine engine;
	return engine;
}

DiffEngine::DiffEngine() : m_matchCache([]() {
	bool valid = false;
	int mib = qEnvironmentVariableIntValue("BINJA_DIFF_PAIR_CACHE_MB", &valid);
	return size_t(valid ? std::clamp(mib, 0, 16384) : 256) * 1024 * 1024;
}(), 128), m_extractionCache([]() {
	bool valid = false;
	int mib = qEnvironmentVariableIntValue("BINJA_DIFF_SNAPSHOT_CACHE_MB", &valid);
	return size_t(valid ? std::clamp(mib, 0, 16384) : 512) * 1024 * 1024;
}(), 64)
{
	// The engine is built and installed as part of this plugin. An installed UI
	// plugin loads only its private engine; an in-place development build prefers
	// the engine beside that build. Parent directories, the legacy top-level copy,
	// and the platform search path are never searched, so a stray or planted
	// library elsewhere cannot be loaded in place of the bundled engine.
	std::vector<std::string> candidates;
	auto addCandidate = [&candidates](const std::string& candidate) {
		if (!candidate.empty()
		    && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
			candidates.push_back(candidate);
	};
	const std::string libraryName = engineLibraryName();
	const QString overridePath = qEnvironmentVariable("BINJA_DIFF_ENGINE_PATH");
	if (!overridePath.isEmpty())
		addCandidate(overridePath.toStdString());
	const std::string userPluginDirectory = BinaryNinja::GetUserPluginDirectory();
	const std::string privateEngine =
	    userPluginDirectory + "/.binja_diff_view/" + libraryName;
	std::string loadedDirectory;
#if defined(Q_OS_UNIX)
	Dl_info info;
	if (dladdr((void*)&DiffEngine::instance, &info) && info.dli_fname)
	{
		loadedDirectory = info.dli_fname;
		size_t slash = loadedDirectory.rfind('/');
		if (slash != std::string::npos)
			loadedDirectory = loadedDirectory.substr(0, slash);
	}
#endif
	const bool installedPlugin = loadedDirectory == userPluginDirectory;
	if (!installedPlugin && !loadedDirectory.empty())
		addCandidate(loadedDirectory + "/" + libraryName);
	addCandidate(privateEngine);
	if (!installedPlugin)
		addCandidate(userPluginDirectory + "/binja_diff_view/build/" + libraryName);

	m_library = std::make_unique<LibraryHolder>();
	std::string lastError;
	for (const auto& candidate : candidates)
	{
		m_library->library.setFileName(QString::fromStdString(candidate));
		m_library->library.setLoadHints(QLibrary::ResolveAllSymbolsHint);
		if (!m_library->library.load())
		{
			lastError = candidate + ": " + m_library->library.errorString().toStdString();
			continue;
		}
		JsonFn blockDiff = reinterpret_cast<JsonFn>(
		    m_library->library.resolve("rust_diff_block_diff_json"));
		JsonFn ilDiff = reinterpret_cast<JsonFn>(
		    m_library->library.resolve("rust_diff_il_diff_json"));
		Json2Fn diffJson = reinterpret_cast<Json2Fn>(
		    m_library->library.resolve("rust_diff_diff_json"));
		Json2CancelFn cancellable = reinterpret_cast<Json2CancelFn>(
		    m_library->library.resolve("rust_diff_diff_json_cancellable"));
		FreeFn freeString = reinterpret_cast<FreeFn>(
		    m_library->library.resolve("rust_diff_free_string"));
		if (blockDiff && ilDiff && diffJson && freeString)
		{
			m_blockDiff = blockDiff;
			m_ilDiff = ilDiff;
			m_diffJson = diffJson;
			m_diffJsonCancellable = cancellable;
			m_freeString = freeString;
			// Optional versioned snapshot API. Older engines retain the JSON path.
			auto prepare = reinterpret_cast<PrepareFn>(m_library->library.resolve("rust_diff_prepare_json_v1"));
			auto release = reinterpret_cast<FreePreparedFn>(m_library->library.resolve("rust_diff_free_prepared_v1"));
			auto size = reinterpret_cast<PreparedSizeFn>(m_library->library.resolve("rust_diff_prepared_size_v1"));
			auto diff = reinterpret_cast<DiffPreparedFn>(m_library->library.resolve("rust_diff_diff_prepared_v1"));
			if (prepare && release && size && diff)
			{
				m_prepareJson = prepare;
				m_freePrepared = release;
				m_preparedSize = size;
				m_diffPrepared = diff;
			}
			break;
		}
		lastError = candidate + ": loaded but missing required rust_diff exports";
		m_library->library.unload();
	}
	if (!m_blockDiff)
	{
		m_loadError = std::string("compatible rust_diff library not found; tried: ");
		for (const auto& candidate : candidates)
			m_loadError += candidate + " ";
		if (!lastError.empty())
			m_loadError += "(" + lastError + ")";
		LogError("binja_diff_view: %s", m_loadError.c_str());
	}
}

DiffEngine::~DiffEngine() = default;

std::string DiffEngine::call(JsonFn fn, const std::string& request)
{
	if (!fn)
		return {};
	char* result = fn(request.c_str());
	if (!result)
		return {};
	std::string out = result;
	m_freeString(result);
	return out;
}

// `func` is non-null only for disassembly graphs, whose line addresses are
// native instructions with code references.
static json linesToJson(const std::vector<DisassemblyTextLine>& lines, Function* func,
    std::vector<uint64_t>& lineAddrs, std::vector<size_t>& lineInstrIndices)
{
	json out = json::array();
	for (const auto& line : lines)
	{
		json tokens = json::array();
		std::string text;
		bool annotationSeen = false;
		// In disassembly, an `adrp` page base renders as whatever symbol starts on
		// that page, possibly as several tokens. Fold it into one address token.
		const bool pageBase = func && !line.tokens.empty()
		    && std::any_of(line.tokens.begin(), line.tokens.end(), [](const InstructionTextToken& token) {
			       return token.type == InstructionToken && token.text == "adrp";
		       });
		bool pageOperandStarted = false;
		for (const auto& tok : line.tokens)
		{
			// An annotation's tail, such as the string Binary Ninja shows for an
			// `adrp` page base, is analysis display without an operand index.
			// Whatever string happens to start on that page differs between
			// builds and would make identical instructions compare as changed.
			if (tok.type == AnnotationToken)
				annotationSeen = true;
			if (tokenIgnoredForDiff(tok.type)
			    || (annotationSeen && tok.operand == static_cast<size_t>(BN_INVALID_OPERAND)))
				continue;
			if (pageBase && tok.operand == 1)
			{
				if (pageOperandStarted)
				{
					tokens.back()["text"] = tokens.back()["text"].get<std::string>() + tok.text;
					text += tok.text;
					continue;
				}
				pageOperandStarted = true;
				tokens.push_back({{"kind", tokenKind(PossibleAddressToken)}, {"text", tok.text}});
				text += tok.text;
				continue;
			}
			const bool pageOffset = func
			    && isResolvedPageOffset(func->GetView(),
			        {func, func->GetArchitecture(), line.addr}, line.tokens, tok);
			tokens.push_back({{"kind", pageOffset ? tokenKind(AddressDisplayToken) : tokenKind(tok.type)},
			    {"text", tok.text}});
			text += tok.text;
		}
		out.push_back({{"tokens", std::move(tokens)}, {"text", std::move(text)}});
		lineAddrs.push_back(line.addr);
		lineInstrIndices.push_back(line.instrIndex);
	}
	return out;
}

std::optional<ExtractedFunction> DiffEngine::extract(
    FunctionRef func, const BinaryNinja::FunctionViewType& viewType)
{
	if (!func)
		return std::nullopt;

	Ref<DisassemblySettings> settings = new DisassemblySettings();
#if BN_CURRENT_CORE_ABI_VERSION >= 180
	Ref<FlowGraph> graph = func->CreateFunctionGraphImmediate(viewType, settings);
#else
	Ref<FlowGraph> graph;
	if (viewType.type == MediumLevelILFunctionGraph)
	{
		Ref<MediumLevelILFunction> il = func->GetMediumLevelIL();
		if (il)
			graph = il->CreateFunctionGraph(settings);
	}
	else
	{
		graph = func->CreateFunctionGraph(viewType, settings);
	}
	// Before CreateFunctionGraphImmediate was added, core-created graphs were
	// populated as part of the asynchronous layout request. Headless callers do
	// not have a UI graph widget to start that request for them.
	if (graph && !graph->HasNodes())
	{
		Ref<FlowGraphLayoutRequest> layout = graph->StartLayout([]() {});
		if (layout)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!layout->IsComplete() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (layout->IsComplete())
				graph = layout->GetGraph();
		}
	}
#endif
	if (!graph)
		return std::nullopt;
	std::vector<Ref<FlowGraphNode>> nodes = graph->GetNodes();
	if (nodes.empty())
		return std::nullopt;

	// Node identity -> extraction index, for resolving edges.
	std::map<BNFlowGraphNode*, size_t> nodeIndex;
	for (size_t i = 0; i < nodes.size(); i++)
		nodeIndex[nodes[i]->GetObject()] = i;

	ExtractedFunction out;
	out.function = func;
	out.viewType = viewType;
	for (size_t i = 0; i < nodes.size(); i++)
	{
		ExtractedBlock block;
		block.index = i;
		block.basicBlock = nodes[i]->GetBasicBlock();
		json lines = linesToJson(nodes[i]->GetLines(),
		    viewType.type == NormalFunctionGraph ? func.GetPtr() : nullptr, block.lineAddrs,
		    block.lineInstrIndices);
		block.linesJson = lines.dump();
		block.firstAddr = block.lineAddrs.empty() ? func->GetStart() : block.lineAddrs.front();
		for (const auto& edge : nodes[i]->GetOutgoingEdges())
		{
			if (!edge.target)
				continue;
			auto it = nodeIndex.find(edge.target->GetObject());
			if (it != nodeIndex.end())
				block.successors.push_back(it->second);
		}
		out.blocks.push_back(std::move(block));
	}

	// FlowGraph node order is not an API guarantee. Prefer the unique root node,
	// and use the function-start line to disambiguate graphs with multiple roots.
	std::optional<size_t> firstRoot;
	for (size_t i = 0; i < nodes.size(); i++)
	{
		if (!nodes[i]->GetIncomingEdges().empty())
			continue;
		if (!firstRoot)
			firstRoot = i;
		if (out.blocks[i].firstAddr == func->GetStart())
		{
			out.entry = i;
			return out;
		}
	}
	out.entry = firstRoot.value_or(0);
	return out;
}

// --- whole-binary function match (rust_diff_diff_json) ---------------------

static std::optional<std::string> stringReferenceText(
	BinaryView* bv, const BNStringReference& reference)
{
	if (reference.length == 0 || reference.length > 4096)
		return std::nullopt;
	DataBuffer data = bv->ReadBuffer(reference.start, reference.length);
	if (!data.GetData() || data.GetLength() == 0)
		return std::nullopt;
	const auto* bytes = static_cast<const uint8_t*>(data.GetData());
	std::string result;
	result.reserve(std::min<size_t>(data.GetLength(), 512));
	if (reference.type == Utf16String)
	{
		for (size_t i = 0; i + 1 < data.GetLength() && result.size() < 512; i += 2)
		{
			uint16_t value = static_cast<uint16_t>(bytes[i])
			    | (static_cast<uint16_t>(bytes[i + 1]) << 8);
			if (value == 0)
				break;
			result.push_back(value >= 0x20 && value <= 0x7e ? static_cast<char>(value) : ' ');
		}
	}
	else
	{
		for (size_t i = 0; i < data.GetLength() && result.size() < 512; i++)
		{
			if (bytes[i] == 0)
				break;
			result.push_back(std::isprint(bytes[i]) ? static_cast<char>(bytes[i]) : ' ');
		}
	}
	while (!result.empty() && result.back() == ' ')
		result.pop_back();
	const size_t useful = std::count_if(result.begin(), result.end(), [](unsigned char value) {
		return std::isalnum(value) != 0;
	});
	return result.size() >= 3 && useful >= 2
	    ? std::optional<std::string>(std::move(result)) : std::nullopt;
}

static std::optional<std::string> referencedString(BinaryView* bv, uint64_t address)
{
	if (!address)
		return std::nullopt;
	BNStringReference reference {};
	if (!bv->GetStringAtAddress(address, reference))
		return std::nullopt;
	return stringReferenceText(bv, reference);
}

static void addStringTarget(BinaryView* bv, uint64_t address,
	std::set<std::string>& strings)
{
	if (auto value = referencedString(bv, address))
		strings.insert(std::move(*value));

	// Resolve common data wrappers, including CFString objects. The data-reference
	// table remains stable when disassembly rendering or annotations change.
	const size_t pointerSize = std::max<size_t>(1, bv->GetAddressSize());
	for (uint64_t target : bv->GetDataReferencesFrom(address, pointerSize * 4))
		if (auto value = referencedString(bv, target))
			strings.insert(std::move(*value));
}

using FunctionStringMap = std::map<uint64_t, std::set<std::string>>;

static FunctionStringMap derivedStringReferences(BinaryView* bv)
{
	FunctionStringMap result;
#if BN_CURRENT_CORE_ABI_VERSION >= 187
	try
	{
		for (const auto& value : bv->GetDerivedStrings())
		{
			std::string text(value.value);
			const size_t useful = std::count_if(text.begin(), text.end(), [](unsigned char byte) {
				return std::isalnum(byte) != 0;
			});
			if (text.size() < 3 || text.size() > 512 || useful < 2)
				continue;
			for (const auto& reference : bv->GetDerivedStringCodeReferences(value))
				if (reference.func)
					result[reference.func->GetStart()].insert(text);
		}
	}
	catch (const std::exception& exception)
	{
		// Derived strings are optional security-triage evidence. Some databases
		// contain a core-derived string record that the C++ API cannot materialize;
		// keep ordinary function matching available without that enrichment.
		LogWarn("binja_diff_view: ignoring derived-string extraction failure: %s",
		    exception.what());
		result.clear();
	}
	catch (...)
	{
		LogWarn("binja_diff_view: ignoring unknown derived-string extraction failure");
		result.clear();
	}
#else
	(void)bv;
#endif
	return result;
}

// Extract one function into the engine's FunctionInfo JSON shape (mirrors the
// rust_diff Python frontend's _extract_function; structure hashes are computed
// engine-side, and only A-vs-B consistency matters, so disassembly-text-based
// mnemonics are fine).
static json matchFunctionToJson(BinaryView* bv, Function* func, DisassemblySettings* settings,
	const FunctionStringMap& derivedStrings)
{
	const Ref<LowLevelILFunction> analysis = materializeFunctionAnalysis(func);
	json blocks = json::array();
	std::set<std::string> referencedStrings;
	if (auto found = derivedStrings.find(func->GetStart()); found != derivedStrings.end())
		referencedStrings = found->second;
	size_t instrCount = 0, byteCount = 0, edgeCount = 0;
	for (auto& bb : func->GetBasicBlocks())
	{
		json blockInstrs = json::array();
		size_t blockInstrCount = 0;
		for (auto& line : bb->GetDisassemblyText(settings))
		{
			std::string mnemonic;
			json operands = json::array();
			json operandTypes = json::array();
			int memoryOperandDepth = 0;
			bool annotationSeen = false;
			for (const auto& tok : line.tokens)
			{
				// An `adrp` operand is a 4 KiB page base, not a reference: the string
				// that happens to start on that page is unrelated to the function.
				// The resolved address arrives through the next instruction's
				// code reference, so `adrp` references are skipped below as well.
				if ((tok.type == DataSymbolToken || tok.type == PossibleAddressToken
				        || tok.type == AddressDisplayToken || tok.type == IntegerToken)
				    && mnemonic != "adrp")
				{
					addStringTarget(bv, tok.value, referencedStrings);
					// Image-relative tables point into sections. Offsets such as
					// `[x8, #0x1c0]` would otherwise land in the Mach-O header and
					// collect load-command strings like `/usr/lib/dyld`.
					if (tok.value <= UINT64_MAX - bv->GetStart()
					    && !bv->GetSectionsAt(bv->GetStart() + tok.value).empty())
						addStringTarget(bv, bv->GetStart() + tok.value, referencedStrings);
				}
				if (tok.type == InstructionToken && mnemonic.empty())
				{
					mnemonic = tok.text;
					continue;
				}
				if (mnemonic.empty())
					continue;
				if (tok.type == AnnotationToken)
					annotationSeen = true;
				if (analysisOnlyMatchToken(tok, annotationSeen))
					continue;
				std::string t = tok.text;
				t.erase(0, t.find_first_not_of(" \t"));
				t.erase(t.find_last_not_of(" \t") + 1);
				// Architecture renderers do not consistently use the dedicated
				// Begin/EndMemoryOperand token types. x86 disassembly, for example,
				// emits '[' and ']' as BraceToken values.
				if (tok.type == BeginMemoryOperandToken
				    || (tok.type == BraceToken && t == "["))
					memoryOperandDepth++;
				else if ((tok.type == EndMemoryOperandToken
				             || (tok.type == BraceToken && t == "]"))
				    && memoryOperandDepth > 0)
					memoryOperandDepth--;
				if (!t.empty() && t != ",")
				{
					operands.push_back(t);
					auto operandType = tok.type;
					if (tok.type == IntegerToken && memoryOperandDepth > 0)
					{
						bool pointsIntoImage = addressResolvesIntoImage(bv, tok.value);
						if (!pointsIntoImage)
							if (auto displayed = displayedIntegerValue(t))
								pointsIntoImage = addressResolvesIntoImage(bv, *displayed);
						if (pointsIntoImage)
							operandType = AddressDisplayToken;
					}
					else if (isResolvedPageOffset(
					             bv, {func, bb->GetArchitecture(), line.addr}, line.tokens, tok))
						operandType = AddressDisplayToken;
					operandTypes.push_back(static_cast<unsigned>(operandType));
				}
			}
			if (mnemonic.empty())
				continue;  // label/annotation line
			if (mnemonic == "adrp" && operands.size() > 1)
			{
				// The page base renders as whatever symbol starts on that page,
				// possibly split into several tokens. Keep it as one address.
				operands = json::array({operands[0], "<page>"});
				operandTypes = json::array({operandTypes[0], static_cast<unsigned>(PossibleAddressToken)});
			}
			if (mnemonic != "adrp")
				for (uint64_t target :
				    bv->GetCodeReferencesFrom({func, bb->GetArchitecture(), line.addr}))
					addStringTarget(bv, target, referencedStrings);
			size_t length = bv->GetInstructionLength(bb->GetArchitecture(), line.addr);
			// Matching consumes mnemonic and operand text. Raw bytes, instruction
			// addresses, and per-instruction lengths were previously serialized but
			// never read by a matching phase. Keep `length` locally for the byte-count
			// result column, while avoiding one ReadBuffer and byte-array allocation
			// per instruction.
			json instr = {{"mnemonic", mnemonic}, {"operands", std::move(operands)},
			    {"operand_types", std::move(operandTypes)}};
			blockInstrs.push_back(std::move(instr));
			blockInstrCount++;
			byteCount += length;
		}
		std::set<uint64_t> edges;
		for (const auto& edge : bb->GetOutgoingEdges())
		{
			if (edge.target)
				edges.insert(edge.target->GetStart());
		}
		edgeCount += edges.size();
		blocks.push_back({{"address", bb->GetStart()}, {"size", bb->GetLength()},
		    {"instructions", std::move(blockInstrs)}, {"edges", edges},
		    {"mnemonic_hash", ""}, {"instruction_count", blockInstrCount}});
		instrCount += blockInstrCount;
	}

	std::set<uint64_t> callees, callers;
	for (const auto& site : func->GetCallSites())
	{
		for (uint64_t target : bv->GetCallees(site))
			callees.insert(target);
	}
	for (const auto& ref : bv->GetCallers(func->GetStart()))
	{
		if (ref.func)
			callers.insert(ref.func->GetStart());
	}

	size_t blockCount = blocks.size();
	uint64_t complexity = (edgeCount + 2 > blockCount) ? (edgeCount + 2 - blockCount) : 1;
	auto sym = func->GetSymbol();
	std::string funcName = functionMatchName(bv, sym);
	return {{"name", funcName}, {"address", func->GetStart()},
	    {"size", byteCount ? byteCount : func->GetHighestAddress() - func->GetStart() + 1},
	    {"basic_blocks", std::move(blocks)},
	    {"cyclomatic_complexity", complexity}, {"call_graph_hash", ""}, {"cfg_hash", ""},
	    {"instruction_count", instrCount}, {"call_count", callees.size()},
	    {"callees", callees}, {"callers", callers}, {"strings", referencedStrings}};
}

// Materializing a lazily restored function is core analysis work that runs
// concurrently for distinct functions; one thread took minutes for a large
// database. Workers extract whole functions so each renders while its analysis
// is held, and results keep the function-list order. An exception escaping a
// std::thread calls std::terminate and takes Binary Ninja down, so workers stop
// the pool and the first exception is rethrown on the calling thread, where the
// controller reports it as a matching failure.
static std::optional<std::vector<json>> extractFunctionsInParallel(BinaryView* bv,
    const std::vector<Ref<Function>>& functions, const FunctionStringMap& derivedStrings,
    Ref<BackgroundTask> task, const char* side)
{
	std::vector<json> extracted(functions.size());
	std::atomic<size_t> next {0}, done {0};
	std::atomic<bool> cancelled {false};
	std::mutex failureMutex;
	std::exception_ptr failure;
	const unsigned workerCount = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
	std::vector<std::thread> workers;
	workers.reserve(workerCount);
	auto joinAll = [&]() {
		for (auto& worker : workers)
			if (worker.joinable())
				worker.join();
	};
	try
	{
		for (unsigned worker = 0; worker < workerCount; worker++)
			workers.emplace_back([&]() {
				try
				{
					Ref<DisassemblySettings> settings = new DisassemblySettings();
					for (size_t index = next++; index < functions.size() && !cancelled; index = next++)
					{
						extracted[index] = matchFunctionToJson(bv, functions[index], settings, derivedStrings);
						done++;
					}
				}
				catch (...)
				{
					std::lock_guard<std::mutex> lock(failureMutex);
					if (!failure)
						failure = std::current_exception();
					cancelled = true;
				}
			});
	}
	catch (...)
	{
		// Thread creation failed; joinable workers must not be destroyed.
		cancelled = true;
		joinAll();
		throw;
	}
	while (done < functions.size() && !cancelled)
	{
		if (task && task->IsCancelled())
			cancelled = true;
		else if (task)
			task->SetProgressText("Binja Diff: extracting binary " + std::string(side) + " ("
			    + std::to_string(done.load()) + "/" + std::to_string(functions.size()) + ")");
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	joinAll();
	if (failure)
		std::rethrow_exception(failure);
	if (cancelled)
		return std::nullopt;
	return extracted;
}

static std::optional<std::string> extractBinaryForMatch(
    BinaryViewRef bv, Ref<BackgroundTask> task, const char* side)
{
	const FunctionStringMap derivedStrings = derivedStringReferences(bv);
	auto extracted = extractFunctionsInParallel(
	    bv, bv->GetAnalysisFunctionList(), derivedStrings, task, side);
	if (!extracted)
		return std::nullopt;
	json functions = json::array();
	for (auto& function : *extracted)
		functions.push_back(std::move(function));
	// Symbol and derived-string data originate in analysis plugins and may contain
	// malformed UTF-8. Replacement encoding keeps one bad label from aborting a
	// whole-binary match; Rust receives valid UTF-8 and the original BNDB is not
	// modified.
	return functions.dump(-1, ' ', false, json::error_handler_t::replace);
}

static std::string hexAddr(uint64_t addr)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
	return buf;
}

DiffEngine::ViewKey DiffEngine::viewKey(BinaryViewRef view)
{
	if (!view)
		return {};
	FileMetadata* file = view->GetFile();
	return {file ? file->GetSessionId() : 0, view->GetTypeName()};
}

// A raw binary is reanalyzed whenever it is opened, so its extraction depends on
// the effective settings as well as the file bytes. A BNDB carries its saved
// analysis and view settings, which its content digest already covers. Settings
// outside analysis only cost a recomputation if they change.
static std::string effectiveSettingsDigest(BinaryViewRef view)
{
	static constexpr std::string_view kIgnoredPrefixes[] = {
	    "ui.", "network.", "updates.", "python.", "pluginManager.", "binjaDiffView."};
	Ref<Settings> settings = Settings::Instance();
	std::vector<std::string> keys = settings->Keys();
	std::sort(keys.begin(), keys.end());
	QCryptographicHash hash(QCryptographicHash::Sha256);
	for (const auto& key : keys)
	{
		if (std::any_of(std::begin(kIgnoredPrefixes), std::end(kIgnoredPrefixes),
		        [&key](std::string_view prefix) { return key.rfind(prefix, 0) == 0; }))
			continue;
		const std::string entry = key + '\0' + settings->GetJson(key, view) + '\n';
		hash.addData(QByteArrayView(entry.data(), static_cast<qsizetype>(entry.size())));
	}
	return hash.result().toHex().toStdString();
}

std::optional<std::string> DiffEngine::persistentViewIdentity(BinaryViewRef view)
{
	if (!view)
		return std::nullopt;
	FileMetadata* file = view->GetFile();
	// A disk cache represents the last saved BNDB. Never use or publish it over
	// unsaved bytes, symbols, types, or auto-analysis changes.
	if (!file || file->IsModified() || file->IsAnalysisChanged()
#if BN_CURRENT_CORE_ABI_VERSION >= 180
	    || view->GetAnalysisState() != IdleState)
#else
	    || view->GetAnalysisInfo().state != IdleState)
#endif
		return std::nullopt;
	QFileInfo info(QString::fromStdString(file->GetFilename()));
	if (!info.exists() || !info.isFile())
		return std::nullopt;
	QString path = info.canonicalFilePath();
	if (path.isEmpty())
		path = info.absoluteFilePath();
	// Saves and cache lookups request the same identities repeatedly on the UI
	// thread; hash each saved file once per change rather than once per request.
	static FileDigestMemo digests;
	auto digest = digests.digest(file->GetSessionId(), info);
	if (!digest)
		return std::nullopt;
	json identity = {{"path", path.toStdString()}, {"size", info.size()},
	    {"mtime_ms", info.lastModified().toMSecsSinceEpoch()},
	    {"content_sha256", digest->toStdString()},
	    {"view_type", view->GetTypeName()}};
	if (!file->IsBackedByDatabase(view->GetTypeName()))
		identity["analysis_settings_sha256"] = effectiveSettingsDigest(view);
	return identity.dump();
}

std::optional<std::string> DiffEngine::persistentIdentity(BinaryViewRef view) const
{
	return persistentViewIdentity(view);
}

// Extracted text and IL come from Binary Ninja's rendering and analysis, which
// can change between core versions while the saved file stays byte-identical.
static const std::string& coreCacheMaterial()
{
	static const std::string material = "binaryninja-core " + BinaryNinja::GetVersionString();
	return material;
}

static std::optional<std::string> matchCachePathForIdentities(
    const std::string& identityA, const std::string& identityB)
{
	if (!diskCacheBudgetBytes()) return std::nullopt;
	std::string material = directionalMatchCacheMaterial(
	    CurrentMatchSchemaVersion, identityA, identityB) + "\n" + coreCacheMaterial();
	QByteArray digest = QCryptographicHash::hash(
	    QByteArray::fromStdString(material), QCryptographicHash::Sha256).toHex();
	QString root = diskCacheRoot();
	if (root.isEmpty())
		return std::nullopt;
	return QDir(root).filePath("binja_diff_view/matches/" + digest + ".json").toStdString();
}

static std::optional<std::string> extractionCachePathForIdentity(const std::string& identity)
{
	if (!diskCacheBudgetBytes()) return std::nullopt;
	std::string material = "binja-diff-extraction-cache\n"
	    + std::to_string(CurrentExtractionSchemaVersion) + "\n" + identity
	    + "\n" + coreCacheMaterial();
	QByteArray digest = QCryptographicHash::hash(
	    QByteArray::fromStdString(material), QCryptographicHash::Sha256).toHex();
	QString root = diskCacheRoot();
	if (root.isEmpty())
		return std::nullopt;
	return QDir(root).filePath("binja_diff_view/extractions/" + digest + ".cache").toStdString();
}

int DiffEngine::matchSchemaVersion()
{
	return CurrentMatchSchemaVersion;
}

int DiffEngine::extractionSchemaVersion()
{
	return CurrentExtractionSchemaVersion;
}

std::optional<std::string> DiffEngine::persistentCachePath(
    BinaryViewRef a, BinaryViewRef b)
{
	if (!diskCacheBudgetBytes()) return std::nullopt;
	auto identityA = persistentViewIdentity(a);
	auto identityB = persistentViewIdentity(b);
	if (!identityA || !identityB)
		return std::nullopt;
	// Security assessment is directional: A is the baseline and B is patched.
	// Keep the durable artifact directional even though the mapping is reversible.
	return matchCachePathForIdentities(*identityA, *identityB);
}

std::optional<std::string> DiffEngine::persistentExtractionCachePath(BinaryViewRef view)
{
	if (!diskCacheBudgetBytes()) return std::nullopt;
	auto identity = persistentViewIdentity(view);
	if (!identity)
		return std::nullopt;
	return extractionCachePathForIdentity(*identity);
}

DiffEngine::BinaryExtractionRef DiffEngine::loadPersistentBinaryExtraction(
    BinaryViewRef view) const
{
	if (!diskCacheBudgetBytes()) return {};
	auto identity = persistentViewIdentity(view);
	auto path = identity ? extractionCachePathForIdentity(*identity) : std::nullopt;
	if (!identity || !path)
		return {};
	QFile file(QString::fromStdString(*path));
	if (!file.open(QIODevice::ReadOnly))
		return {};
	if (static_cast<uint64_t>(file.size()) > diskCacheBudgetBytes())
		return {};
	try
	{
		QByteArray contents = file.readAll();
		qsizetype first = contents.indexOf('\n');
		qsizetype second = first < 0 ? -1 : contents.indexOf('\n', first + 1);
		qsizetype third = second < 0 ? -1 : contents.indexOf('\n', second + 1);
		if (first < 0 || second < 0 || third < 0
		    || contents.left(first) != kExtractionCacheMagic)
			throw std::runtime_error("invalid extraction cache header");
		QByteArray storedIdentity = QByteArray::fromBase64(contents.mid(first + 1, second - first - 1));
		if (storedIdentity.toStdString() != *identity)
			throw std::runtime_error("extraction cache identity mismatch");
		QByteArray expectedDigest = contents.mid(second + 1, third - second - 1);
		QByteArray payload = contents.mid(third + 1);
		QByteArray actualDigest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
		if (expectedDigest != actualDigest)
			throw std::runtime_error("extraction cache payload checksum mismatch");
		LogInfo("binja_diff_view: restored persistent function extraction for %s",
		    view->GetTypeName().c_str());
		touchDiskCacheArtifact(file);
		return std::make_shared<BinaryExtraction>(payload.toStdString());
	}
	catch (const std::exception& exception)
	{
		LogWarn("binja_diff_view: ignoring invalid persistent extraction cache %s: %s",
		    path->c_str(), exception.what());
		file.close();
		QFile::remove(QString::fromStdString(*path));
		return {};
	}
}

void DiffEngine::storePersistentBinaryExtraction(BinaryViewRef view,
    const std::string& extraction, const PersistenceCheck& stillCurrent) const
{
	if (!diskCacheBudgetBytes()) return;
	auto identity = persistentViewIdentity(view);
	auto path = identity ? extractionCachePathForIdentity(*identity) : std::nullopt;
	if (!identity || !path)
		return;
	QFileInfo info(QString::fromStdString(*path));
	if (!QDir().mkpath(info.absolutePath()))
		return;
	QByteArray payload = QByteArray::fromStdString(extraction);
	QByteArray contents = QByteArray(kExtractionCacheMagic) + '\n'
	    + QByteArray::fromStdString(*identity).toBase64() + '\n'
	    + QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() + '\n'
	    + payload;
	if (static_cast<uint64_t>(contents.size()) > diskCacheBudgetBytes()) return;
	QSaveFile file(info.absoluteFilePath());
	if (!file.open(QIODevice::WriteOnly))
		return;
	if (file.write(contents) != contents.size())
	{
		file.cancelWriting();
		LogWarn("binja_diff_view: could not persist function extraction to %s", path->c_str());
	}
	else if (commitIfCurrent(file, stillCurrent))
		trimDiskCacheForArtifact(info.absoluteFilePath());
}

DiffEngine::BinaryExtractionRef DiffEngine::extractBinaryForMatchCached(
    BinaryViewRef view, Ref<BackgroundTask> task, const char* side)
{
	if (!view)
		return {};
	ViewKey key = viewKey(view);
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		if (auto cached = m_extractionCache.get(key)) return *cached;
	}
	auto work = m_extractionWork.acquire(key, [&]() { return task && task->IsCancelled(); });
	if (!work) return {};
	uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		if (auto cached = m_extractionCache.get(key))
			return *cached;
		epoch = m_viewEpoch[key];
	}
	if (task)
		task->SetProgressText("Binja Diff: checking cached binary " + std::string(side) + "…");
	if (auto persisted = loadPersistentBinaryExtraction(view))
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		if (m_viewEpoch[key] == epoch)
		{
			m_extractionCache.put(key, persisted, sizeof(BinaryExtraction) + persisted->json.capacity());
			return persisted;
		}
		return {};
	}

	auto extracted = extractBinaryForMatch(view, task, side);
	if (!extracted)
		return {};
	auto snapshot = std::make_shared<BinaryExtraction>(std::move(*extracted));
	bool publish = false;
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		if (m_viewEpoch[key] == epoch)
		{
			m_extractionCache.put(key, snapshot, sizeof(BinaryExtraction) + snapshot->json.capacity());
			publish = true;
		}
	}
	if (!publish)
		return {};
	storePersistentBinaryExtraction(
	    view, snapshot->json, [this, key, epoch]() { return viewEpoch(key) == epoch; });
	return snapshot;
}

std::shared_ptr<void> DiffEngine::prepareBinaryCached(BinaryViewRef view,
	const BinaryExtractionRef& extraction, Ref<BackgroundTask> task)
{
	// Single preparation per extraction object, without holding the engine lock.
	// Waiting callers can cancel while another worker prepares the same snapshot.
	std::unique_lock<std::timed_mutex> preparation(extraction->prepareMutex, std::defer_lock);
	while (!preparation.try_lock_for(std::chrono::milliseconds(50)))
		if (task && task->IsCancelled()) return {};
	if (task && task->IsCancelled()) return {};
	if (extraction->prepared) return extraction->prepared;
	if (task) task->SetProgressText("Binja Diff: preparing reusable matching features…");
	auto cancelled = [](void* context) {
		auto* current = static_cast<BackgroundTask*>(context);
		return current && current->IsCancelled();
	};
	void* raw = m_prepareJson(extraction->json.c_str(), cancelled, task.GetPtr());
	if (!raw) return {};
	// Capture the release function, not the engine or the BinaryView.
	extraction->prepared = std::shared_ptr<void>(raw, m_freePrepared);
	const size_t bytes = sizeof(BinaryExtraction) + extraction->json.capacity() + m_preparedSize(raw);
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		m_extractionCache.updateWeight(viewKey(view), extraction, bytes);
	}
	return extraction->prepared;
}


std::optional<FunctionMatchMaps> DiffEngine::loadPersistentFunctionMatches(
	BinaryViewRef a, BinaryViewRef b) const
{
	if (!diskCacheBudgetBytes()) return std::nullopt;
	auto identityA = persistentViewIdentity(a);
	auto identityB = persistentViewIdentity(b);
	auto path = identityA && identityB
	    ? matchCachePathForIdentities(*identityA, *identityB) : std::nullopt;
	if (!identityA || !identityB || !path)
		return std::nullopt;
	QFile file(QString::fromStdString(*path));
	if (!file.open(QIODevice::ReadOnly))
		return std::nullopt;
	if (static_cast<uint64_t>(file.size()) > diskCacheBudgetBytes())
		return std::nullopt;
	try
	{
		json root = json::parse(file.readAll().toStdString());
		if (!root.is_object()
		    || root.value("schema", 0) != CurrentMatchSchemaVersion
		    || root.value("identity_a", "") != *identityA
		    || root.value("identity_b", "") != *identityB
		    || root.value("security_baseline_identity", "") != *identityA
		    || root.value("security_patched_identity", "") != *identityB
		    || !root.contains("result"))
			throw std::runtime_error("persistent match cache identity mismatch");
		FunctionMatchMaps maps = functionMatchMapsFromJson(root["result"]);
		maps.restoredFromDisk = true;
		touchDiskCacheArtifact(file);
		LogInfo("binja_diff_view: restored persistent function matches for %s and %s",
		    a->GetTypeName().c_str(), b->GetTypeName().c_str());
		return maps;
	}
	catch (const std::exception& exception)
	{
		LogWarn("binja_diff_view: ignoring invalid persistent match cache %s: %s",
		    path->c_str(), exception.what());
		file.close();
		QFile::remove(QString::fromStdString(*path));
		return std::nullopt;
	}
}

void DiffEngine::storePersistentFunctionMatches(BinaryViewRef a, BinaryViewRef b,
	const FunctionMatchMaps& maps, const PersistenceCheck& stillCurrent) const
{
	if (!maps.valid || !diskCacheBudgetBytes())
		return;
	auto identityA = persistentViewIdentity(a);
	auto identityB = persistentViewIdentity(b);
	auto path = identityA && identityB
	    ? matchCachePathForIdentities(*identityA, *identityB) : std::nullopt;
	if (!identityA || !identityB || !path)
		return;
	json root = {{"schema", CurrentMatchSchemaVersion},
	    {"identity_a", *identityA}, {"identity_b", *identityB},
	    {"security_baseline_identity", *identityA},
	    {"security_patched_identity", *identityB},
	    {"result", functionMatchMapsToJson(maps)}};
	QFileInfo info(QString::fromStdString(*path));
	if (!QDir().mkpath(info.absolutePath()))
		return;
	QSaveFile file(info.absoluteFilePath());
	if (!file.open(QIODevice::WriteOnly))
		return;
	QByteArray contents = QByteArray::fromStdString(root.dump());
	if (static_cast<uint64_t>(contents.size()) > diskCacheBudgetBytes())
	{
		file.cancelWriting();
		return;
	}
	if (file.write(contents) != contents.size())
	{
		file.cancelWriting();
		LogWarn("binja_diff_view: could not persist function matches to %s", path->c_str());
	}
	else if (commitIfCurrent(file, stillCurrent))
		trimDiskCacheForArtifact(info.absoluteFilePath());
}

bool DiffEngine::commitIfCurrent(QSaveFile& file, const PersistenceCheck& stillCurrent) const
{
	std::lock_guard<std::mutex> lock(m_matchMutex);
	if (!stillCurrent())
	{
		file.cancelWriting();
		return false;
	}
	if (file.commit())
		return true;
	LogWarn("binja_diff_view: could not commit cache artifact %s",
	    file.fileName().toStdString().c_str());
	return false;
}

std::optional<FunctionMatchMaps> DiffEngine::cachedFunctionMatches(
    BinaryViewRef a, BinaryViewRef b) const
{
	auto maps = cachedFunctionMatchesRef(a, b);
	return maps ? std::optional<FunctionMatchMaps>(*maps) : std::nullopt;
}

FunctionMatchMapsRef DiffEngine::cachedFunctionMatchesRef(
    BinaryViewRef a, BinaryViewRef b) const
{
	if (!a || !b)
		return {};
	const MatchKey key = {viewKey(a), viewKey(b)};
	uint64_t epochA = 0, epochB = 0, cacheRevision = 0;
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		if (auto cached = m_matchCache.get(key)) return cached;
		cacheRevision = m_matchCache.revision();
		auto epochItA = m_viewEpoch.find(key.first);
		auto epochItB = m_viewEpoch.find(key.second);
		epochA = epochItA == m_viewEpoch.end() ? 0 : epochItA->second;
		epochB = epochItB == m_viewEpoch.end() ? 0 : epochItB->second;
	}
	auto persisted = loadPersistentFunctionMatches(a, b);
	if (!persisted)
		return {};
	auto snapshot = std::make_shared<FunctionMatchMaps>(std::move(*persisted));
	const size_t bytes = estimatedMatchBytes(*snapshot);
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		auto epochItA = m_viewEpoch.find(key.first);
		auto epochItB = m_viewEpoch.find(key.second);
		uint64_t currentA = epochItA == m_viewEpoch.end() ? 0 : epochItA->second;
		uint64_t currentB = epochItB == m_viewEpoch.end() ? 0 : epochItB->second;
		if (currentA != epochA || currentB != epochB || m_matchCache.revision() != cacheRevision)
			return {};
		if (auto current = m_matchCache.get(key)) return current;
		m_matchCache.put(key, snapshot, bytes);
	}
	return snapshot;
}

std::optional<PreparedFunctionMatches> DiffEngine::prepareCachedFunctionMatches(
    BinaryViewRef a, BinaryViewRef b) const
{
	if (!a || !b)
		return std::nullopt;
	FunctionMatchMapsRef maps = cachedFunctionMatchesRef(a, b);
	if (!maps)
		return std::nullopt;
	const MatchKey key = {viewKey(a), viewKey(b)};
	std::lock_guard<std::mutex> lock(m_matchMutex);
	if (!m_matchCache.current(key, maps))
		return std::nullopt;
	auto epochA = m_viewEpoch.find(key.first);
	auto epochB = m_viewEpoch.find(key.second);
	return PreparedFunctionMatches {maps,
	    key.first.sessionId, key.first.typeName,
	    key.second.sessionId, key.second.typeName,
	    epochA == m_viewEpoch.end() ? 0 : epochA->second,
	    epochB == m_viewEpoch.end() ? 0 : epochB->second};
}

bool DiffEngine::preparedFunctionMatchesCurrent(BinaryViewRef a, BinaryViewRef b,
    const PreparedFunctionMatches& prepared) const
{
	if (!a || !b || !prepared.matches)
		return false;
	const MatchKey key = {viewKey(a), viewKey(b)};
	if (key.first.sessionId != prepared.sessionIdA
	    || key.first.typeName != prepared.typeNameA
	    || key.second.sessionId != prepared.sessionIdB
	    || key.second.typeName != prepared.typeNameB)
		return false;
	std::lock_guard<std::mutex> lock(m_matchMutex);
	if (!m_matchCache.current(key, prepared.matches))
		return false;
	auto epochA = m_viewEpoch.find(key.first);
	auto epochB = m_viewEpoch.find(key.second);
	uint64_t currentA = epochA == m_viewEpoch.end() ? 0 : epochA->second;
	uint64_t currentB = epochB == m_viewEpoch.end() ? 0 : epochB->second;
	return currentA == prepared.epochA && currentB == prepared.epochB;
}

bool DiffEngine::replaceFunctionMatchesIfCurrent(BinaryViewRef a, BinaryViewRef b,
    const FunctionMatchMapsRef& expected, const FunctionMatchMaps& replacement)
{
	return bool(replaceFunctionMatchesIfCurrentRef(a, b, expected, replacement));
}

FunctionMatchMapsRef DiffEngine::replaceFunctionMatchesIfCurrentRef(BinaryViewRef a, BinaryViewRef b,
    const FunctionMatchMapsRef& expected, const FunctionMatchMaps& replacement)
{
	if (!a || !b || !expected || !replacement.valid)
		return {};
	const MatchKey key = {viewKey(a), viewKey(b)};
	const MatchKey reverse = {key.second, key.first};
	auto snapshot = std::make_shared<const FunctionMatchMaps>(replacement);
	const size_t bytes = estimatedMatchBytes(*snapshot);
	std::lock_guard<std::mutex> lock(m_matchMutex);
	if (!m_matchCache.current(key, expected))
		return {};
	m_matchCache.erase(key);
	m_matchCache.put(key, snapshot, bytes);
	m_matchCache.erase(reverse);
	return snapshot;
}

bool DiffEngine::persistCachedFunctionMatches(BinaryViewRef a, BinaryViewRef b) const
{
	if (!a || !b)
		return false;
	const MatchKey key = {viewKey(a), viewKey(b)};
	FunctionMatchMapsRef maps;
	uint64_t epochA = 0, epochB = 0, pairGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		maps = m_matchCache.get(key);
		epochA = viewEpoch(key.first);
		epochB = viewEpoch(key.second);
		pairGeneration = pairInvalidation(key);
	}
	if (!maps || !maps->valid || !persistentCachePath(a, b))
		return false;
	storePersistentFunctionMatches(a, b, *maps, [this, key, epochA, epochB, pairGeneration]() {
		return viewEpoch(key.first) == epochA && viewEpoch(key.second) == epochB
		    && pairInvalidation(key) == pairGeneration;
	});
	return true;
}

bool DiffEngine::persistCachedBinaryExtraction(BinaryViewRef view) const
{
	if (!view)
		return false;
	const ViewKey key = viewKey(view);
	BinaryExtractionRef extraction;
	uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		if (auto cached = m_extractionCache.get(key))
			extraction = *cached;
		epoch = viewEpoch(key);
	}
	if (!extraction || !persistentExtractionCachePath(view))
		return false;
	storePersistentBinaryExtraction(
	    view, extraction->json, [this, key, epoch]() { return viewEpoch(key) == epoch; });
	return true;
}

void DiffEngine::invalidateMatches(BinaryViewRef view)
{
	if (!view)
		return;
	const ViewKey changed = viewKey(view);
	std::lock_guard<std::mutex> lock(m_matchMutex);
	m_viewEpoch[changed]++;
	m_extractionCache.erase(changed);
	m_matchCache.eraseIf([&](const MatchKey& key) {
		return key.first == changed || key.second == changed;
	});
}

void DiffEngine::invalidateExtraction(BinaryViewRef view)
{
	if (!view)
		return;
	const ViewKey changed = viewKey(view);
	std::lock_guard<std::mutex> lock(m_matchMutex);
	m_viewEpoch[changed]++;
	m_extractionCache.erase(changed);
}

void DiffEngine::invalidateMatchPair(BinaryViewRef a, BinaryViewRef b)
{
	if (!a || !b)
		return;
	const MatchKey key = {viewKey(a), viewKey(b)};
	const auto path = persistentCachePath(a, b);
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		m_matchCache.erase(key);
		m_pairInvalidation[key]++;
		// A new cache lookup must not observe the artifact between memory
		// invalidation and removal. Already-running loads recheck the revision.
		if (path) QFile::remove(QString::fromStdString(*path));
	}
}

FunctionMatchMaps DiffEngine::matchFunctions(
    BinaryViewRef a, BinaryViewRef b, Ref<BackgroundTask> task)
{
	return *matchFunctionsRef(a, b, task);
}

FunctionMatchMapsRef DiffEngine::matchFunctionsRef(
    BinaryViewRef a, BinaryViewRef b, Ref<BackgroundTask> task)
{
	QElapsedTimer totalTimer;
	totalTimer.start();
	FunctionMatchMaps failure;
	if (!available() || !a || !b)
	{
		failure.valid = false;
		failure.error = available() ? "Missing binary view" : m_loadError;
		return std::make_shared<const FunctionMatchMaps>(std::move(failure));
	}
	const MatchKey key = {viewKey(a), viewKey(b)};
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		if (auto cached = m_matchCache.get(key)) return cached;
	}
	// Lock order is pair work -> extraction work -> cache; neither work gate
	// is acquired while holding the cache mutex. Separate pairs may run together.
	auto work = m_matchWork.acquire(key, [&]() { return task && task->IsCancelled(); });
	if (!work)
	{
		failure.valid = false;
		failure.error = "Function matching cancelled";
		return std::make_shared<const FunctionMatchMaps>(std::move(failure));
	}
	if (auto cached = cachedFunctionMatchesRef(a, b)) return cached;
	uint64_t epochA = 0, epochB = 0, cacheRevision = 0, pairGeneration = 0;
	bool published = false;
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		if (auto cached = m_matchCache.get(key)) return cached;
		cacheRevision = m_matchCache.revision();
		epochA = m_viewEpoch[key.first];
		epochB = m_viewEpoch[key.second];
		pairGeneration = pairInvalidation(key);

	}

	QElapsedTimer phaseTimer;
	phaseTimer.start();
	auto jsonA = extractBinaryForMatchCached(a, task, "A");
	auto jsonB = jsonA ? extractBinaryForMatchCached(b, task, "B") : BinaryExtractionRef {};
	qint64 extractionMs = phaseTimer.elapsed();
	if (!jsonA || !jsonB || (task && task->IsCancelled()))
	{
		failure.valid = false;
		// Extraction can also decline to publish when a BinaryView notification
		// advances its epoch during the run. That is a transient stale-snapshot
		// condition, not a user cancellation. The workspace controller recognizes
		// this message and retries after analysis has remained idle.
		failure.error = task && task->IsCancelled()
		    ? "Function matching cancelled"
		    : "Analysis changed while function matching was running; retry when analysis settles";
		return std::make_shared<const FunctionMatchMaps>(std::move(failure));
	}
	if (task)
		task->SetProgressText("Binja Diff: matching functions…");
	phaseTimer.restart();
	auto cancelled = [](void* context) {
		auto* backgroundTask = static_cast<BackgroundTask*>(context);
		return backgroundTask && backgroundTask->IsCancelled();
	};
	char* raw = nullptr;
	if (m_diffPrepared)
	{
		auto preparedA = prepareBinaryCached(a, jsonA, task);
		auto preparedB = preparedA ? prepareBinaryCached(b, jsonB, task) : std::shared_ptr<void> {};
		if (preparedA && preparedB)
			raw = m_diffPrepared(preparedA.get(), preparedB.get(), cancelled, task.GetPtr());
	}
	else
		raw = (m_diffJsonCancellable && task)
			? m_diffJsonCancellable(jsonA->json.c_str(), jsonB->json.c_str(), cancelled, task.GetPtr())
			: m_diffJson(jsonA->json.c_str(), jsonB->json.c_str());
	if (!raw)
	{
		LogWarn("binja_diff_view: function match run failed; callee renames will read as changes");
		failure.valid = false;
		failure.error = task && task->IsCancelled()
		    ? "Function matching cancelled"
		    : "rust_diff function match returned no response";
		return std::make_shared<const FunctionMatchMaps>(std::move(failure));
	}
	std::string response = raw;
	m_freeString(raw);
	qint64 engineMs = phaseTimer.elapsed();
	phaseTimer.restart();

	FunctionMatchMaps maps;
	try
	{
		json r = json::parse(response);
		if (!r.is_object() || !r.contains("matched_functions") || !r["matched_functions"].is_array())
			throw std::runtime_error(
			    "function-match response requires an object with matched_functions[]");
		auto summaryFromJson = [](const json& f) {
			FunctionSummary summary;
			summary.name = f.value("name", "");
			summary.address = f.value("address", 0ull);
			summary.size = f.value("size", 0ull);
			summary.instructionCount = f.value("instruction_count", size_t {0});
			if (f.contains("suggested_counterpart") && f["suggested_counterpart"].is_object())
			{
				const auto& suggestion = f["suggested_counterpart"];
				summary.suggestedCounterpartAddress = suggestion.value("address", uint64_t {0});
				summary.suggestedCounterpartName = suggestion.value("name", "");
				summary.suggestedBodySimilarity = suggestion.value("body_similarity", 0.0);
			}
			if (f.contains("security"))
				securityAssessmentFromJson(f["security"], summary);
			return summary;
		};
		maps.similarity = r.value("similarity_score", 0.0);
		maps.matchedSimilarity = r.value("matched_similarity_score", 0.0);
		maps.coverage = r.value("match_coverage", 0.0);
		maps.analysisTime = r.value("analysis_time", 0.0);
		for (const auto& match : r["matched_functions"])
		{
			if (!match.is_object() || !match.contains("function_a") || !match["function_a"].is_object()
			    || !match.contains("function_b") || !match["function_b"].is_object())
				throw std::runtime_error("invalid matched_functions entry");
			const auto& fa = match["function_a"];
			const auto& fb = match["function_b"];
			uint64_t addrA = fa.value("address", 0ull);
			uint64_t addrB = fb.value("address", 0ull);
			FunctionMatchRecord record;
			record.a = summaryFromJson(fa);
			record.b = summaryFromJson(fb);
			record.similarity = match.value("similarity", 0.0);
			record.confidence = match.value("confidence", 0.0);
			record.ambiguityMargin = match.value("ambiguity_margin", 1.0);
			record.matchType = match.value("match_type", "Unknown");
			record.contentMatch = match.value("content_match", "Changed");
			if (match.contains("security"))
				securityAssessmentFromJson(match["security"], record);
			if (match.contains("details") && match["details"].is_object())
			{
				const auto& details = match["details"];
				record.cfgSimilarity = details.value("cfg_similarity", 0.0);
				record.basicBlockSimilarity = details.value("bb_similarity", 0.0);
				record.instructionSimilarity = details.value("instruction_similarity", 0.0);
				record.edgeSimilarity = details.value("edge_similarity", 0.0);
				record.nameSimilarity = details.value("name_similarity", 0.0);
				record.callSimilarity = details.value("call_similarity", 0.0);
				record.neighborSupport = details.value("neighbor_support", size_t(0));
				record.neighborContradictions = details.value("neighbor_contradictions", size_t(0));
				record.bindingAnchor = details.value("binding_anchor", false);
			}
			maps.matches.push_back(std::move(record));
			if (addrA != addrB)
				maps.addrMap[hexAddr(addrA)] = hexAddr(addrB);
			maps.funcMap[addrA] = addrB;
			maps.matchCount++;
		}
		if (!r.contains("unmatched_functions_a") || !r["unmatched_functions_a"].is_array()
		    || !r.contains("unmatched_functions_b") || !r["unmatched_functions_b"].is_array())
			throw std::runtime_error("function-match response requires unmatched function arrays");
		for (const auto& function : r["unmatched_functions_a"])
			maps.unmatchedA.push_back(summaryFromJson(function));
		for (const auto& function : r["unmatched_functions_b"])
			maps.unmatchedB.push_back(summaryFromJson(function));
		rebuildCalleeRenameMap(maps);
		if (r.contains("review_suggestions") && r["review_suggestions"].is_array())
		{
			std::map<uint64_t, FunctionSummary*> unmatchedA, unmatchedB;
			for (auto& function : maps.unmatchedA)
				unmatchedA[function.address] = &function;
			for (auto& function : maps.unmatchedB)
				unmatchedB[function.address] = &function;
			for (const auto& suggestion : r["review_suggestions"])
			{
				if (!suggestion.is_object())
					continue;
				uint64_t addressA = suggestion.value("a_address", uint64_t {0});
				uint64_t addressB = suggestion.value("b_address", uint64_t {0});
				auto a = unmatchedA.find(addressA), b = unmatchedB.find(addressB);
				if (a == unmatchedA.end() || b == unmatchedB.end())
					continue;
				double similarity = suggestion.value("body_similarity", 0.0);
				a->second->suggestedCounterpartAddress = addressB;
				a->second->suggestedCounterpartName = b->second->name;
				a->second->suggestedBodySimilarity = similarity;
				b->second->suggestedCounterpartAddress = addressA;
				b->second->suggestedCounterpartName = a->second->name;
				b->second->suggestedBodySimilarity = similarity;
			}
		}
	}
	catch (const std::exception& e)
	{
		maps.valid = false;
		maps.error = std::string("Invalid rust_diff function-match response: ") + e.what();
		LogError("binja_diff_view: %s", maps.error.c_str());
	}
	LogInfo("binja_diff_view: matched %zu function pairs (%zu renames, %zu relocations)",
	    maps.matchCount, maps.renameMap.size(), maps.addrMap.size());
	FunctionMatchMapsRef snapshot;
	const size_t bytes = estimatedMatchBytes(maps);
	{
		std::lock_guard<std::mutex> lock(m_matchMutex);
		// Do not publish a snapshot produced while analysis was changing. The
		// caller should retry once analysis has settled.
		if (maps.valid && m_viewEpoch[key.first] == epochA && m_viewEpoch[key.second] == epochB
		    && m_matchCache.revision() == cacheRevision)
		{
			snapshot = m_matchCache.get(key);
			if (!snapshot)
			{
				snapshot = std::make_shared<const FunctionMatchMaps>(std::move(maps));
				m_matchCache.put(key, snapshot, bytes);
				published = true;
			}
		}
		else if (maps.valid)
		{
			maps.valid = false;
			maps.error = "Analysis changed while function matching was running; retry when analysis settles";
		}
	}
	if (published)
		storePersistentFunctionMatches(a, b, *snapshot, [this, key, epochA, epochB, pairGeneration]() {
			return viewEpoch(key.first) == epochA && viewEpoch(key.second) == epochB
			    && pairInvalidation(key) == pairGeneration;
		});
	LogInfo("binja_diff_view: cold match timing extraction=%lld ms engine=%lld ms "
	        "decode+persist=%lld ms total=%lld ms (request=%zu+%zu bytes, response=%zu bytes)",
	    static_cast<long long>(extractionMs), static_cast<long long>(engineMs),
	    static_cast<long long>(phaseTimer.elapsed()), static_cast<long long>(totalTimer.elapsed()),
	    jsonA->json.size(), jsonB->json.size(), response.size());
	return snapshot ? snapshot : std::make_shared<const FunctionMatchMaps>(std::move(maps));
}

static json functionToJson(const ExtractedFunction& f)
{
	json blocks = json::array();
	for (const auto& b : f.blocks)
	{
		blocks.push_back({{"index", b.index},
		    {"lines", json::parse(b.linesJson)},
		    {"successors", b.successors}});
	}
	return {{"level", f.viewType.name.empty() ? "IL" : f.viewType.name},
	    {"blocks", std::move(blocks)}, {"entry", f.entry}};
}

static LineDiff lineDiffFromJson(const json& value)
{
	if (!value.is_object() || !value.contains("lines") || !value["lines"].is_array())
		throw std::runtime_error("missing lines array");
	auto parseSpans = [](const json& line, const char* key) {
		std::vector<TokenSpan> spans;
		if (!line.contains(key))
			return spans;
		if (!line[key].is_array())
			throw std::runtime_error(std::string(key) + " must be an array");
		for (const auto& span : line[key])
		{
			if (!span.is_object())
				throw std::runtime_error("invalid token span");
			spans.push_back({span.value("text", ""), span.value("changed", false)});
		}
		return spans;
	};
	auto parseText = [](const json& line, const char* key) {
		return line.contains(key) && line[key].is_string()
		    ? line[key].get<std::string>() : std::string {};
	};
	LineDiff out;
	for (const auto& line : value["lines"])
	{
		if (!line.is_object() || !line.contains("op") || !line["op"].is_string())
			throw std::runtime_error("invalid line diff entry");
		std::string opName = line["op"].get<std::string>();
		LineOp op;
		if (opName == "equal")
			op = LineOp::Equal;
		else if (opName == "rename")
			op = LineOp::Rename;
		else if (opName == "replace")
			op = LineOp::Replace;
		else if (opName == "insert")
			op = LineOp::Insert;
		else if (opName == "delete")
			op = LineOp::Delete;
		else
			throw std::runtime_error("unknown line op: " + opName);
		out.lines.push_back({op, parseText(line, "a"), parseText(line, "b"),
		    parseSpans(line, "a_spans"), parseSpans(line, "b_spans")});
	}
	return out;
}

std::optional<BlockDiffResult> DiffEngine::blockDiff(const ExtractedFunction& a,
    const ExtractedFunction& b, const FunctionMatchMaps& maps)
{
	if (!available())
		return std::nullopt;
	double threshold = 0.5;
	try
	{
		Ref<Settings> settings = BinaryNinja::Settings::Instance();
		if (settings->Contains("binjaDiffView.blockMatchThreshold"))
			threshold = settings->Get<double>("binjaDiffView.blockMatchThreshold");
	}
	catch (...)
	{}
	json request = {{"a", functionToJson(a)}, {"b", functionToJson(b)},
	    {"rename_map", maps.renameMap}, {"addr_map", maps.addrMap},
	    {"threshold", threshold}};
	std::string response = call(m_blockDiff, request.dump());
	if (response.empty())
		return std::nullopt;

	try
	{
		json r = json::parse(response);
		if (!r.is_object() || !r.contains("pairs") || !r["pairs"].is_array()
		    || !r.contains("only_a") || !r["only_a"].is_array()
		    || !r.contains("only_b") || !r["only_b"].is_array())
			throw std::runtime_error("missing pairs/only_a/only_b arrays");

		BlockDiffResult out;
		for (const auto& p : r["pairs"])
		{
			if (!p.is_object() || !p.contains("a") || !p["a"].is_number_unsigned()
			    || !p.contains("b") || !p["b"].is_number_unsigned()
			    || !p.contains("status") || !p["status"].is_string())
				throw std::runtime_error("invalid block pair entry");
			BlockStatus status;
			std::string s = p["status"].get<std::string>();
			if (s == "equal")
				status = BlockStatus::Equal;
			else if (s == "cosmetic")
				status = BlockStatus::Cosmetic;
			else if (s == "changed")
				status = BlockStatus::Changed;
			else
				throw std::runtime_error("unknown block status: " + s);
			out.pairs.push_back({p["a"].get<size_t>(), p["b"].get<size_t>(), status,
			    p.value("similarity", 0.0)});
		}
		out.onlyA = r["only_a"].get<std::vector<size_t>>();
		out.onlyB = r["only_b"].get<std::vector<size_t>>();
		if (r.contains("line_diffs"))
		{
			if (!r["line_diffs"].is_array())
				throw std::runtime_error("line_diffs must be an array");
			for (const auto& entry : r["line_diffs"])
			{
				if (!entry.is_object() || !entry.contains("a") || !entry["a"].is_number_unsigned()
				    || !entry.contains("b") || !entry["b"].is_number_unsigned()
				    || !entry.contains("diff"))
					throw std::runtime_error("invalid line_diffs entry");
				size_t blockA = entry["a"].get<size_t>();
				size_t blockB = entry["b"].get<size_t>();
				out.lineDiffs[{blockA, blockB}] = lineDiffFromJson(entry["diff"]);
			}
		}
		if (r.contains("il_similarity") && r["il_similarity"].is_number())
			out.ilSimilarity = r["il_similarity"].get<double>();
		out.similarity = r.value("similarity", 0.0);
		return out;
	}
	catch (const std::exception& e)
	{
		LogError("binja_diff_view: invalid rust_diff block response: %s", e.what());
		return std::nullopt;
	}
}

std::optional<LineDiff> DiffEngine::lineDiff(const ExtractedBlock& a, const ExtractedBlock& b,
    const FunctionMatchMaps& maps)
{
	if (!available())
		return std::nullopt;
	json request = {
	    {"a", {{"level", "IL"}, {"lines", json::parse(a.linesJson)}}},
	    {"b", {{"level", "IL"}, {"lines", json::parse(b.linesJson)}}},
	    {"rename_map", maps.renameMap},
	    {"addr_map", maps.addrMap},
	};
	std::string response = call(m_ilDiff, request.dump());
	if (response.empty())
		return std::nullopt;
	try
	{
		return lineDiffFromJson(json::parse(response));
	}
	catch (const std::exception& e)
	{
		LogError("binja_diff_view: invalid rust_diff line response: %s", e.what());
		return std::nullopt;
	}
}

std::optional<double> DiffEngine::ilSimilarity(const ExtractedFunction& a, const ExtractedFunction& b,
    const FunctionMatchMaps& maps)
{
	if (!available())
		return std::nullopt;

	// Concatenate every block's lines in display order into one token stream per
	// function, matching how the rust_diff text view scores a whole function.
	auto concatLines = [](const ExtractedFunction& f) {
		json lines = json::array();
		for (const auto& block : f.blocks)
		{
			json blockLines = json::parse(block.linesJson, nullptr, false);
			if (blockLines.is_array())
			{
				for (auto& line : blockLines)
					lines.push_back(std::move(line));
			}
		}
		return lines;
	};

	json request = {
	    {"a", {{"level", "IL"}, {"lines", concatLines(a)}}},
	    {"b", {{"level", "IL"}, {"lines", concatLines(b)}}},
	    {"rename_map", maps.renameMap},
	    {"addr_map", maps.addrMap},
	};
	std::string response = call(m_ilDiff, request.dump());
	if (response.empty())
		return std::nullopt;
	try
	{
		json r = json::parse(response);
		if (!r.is_object() || !r.contains("similarity") || !r["similarity"].is_number())
			throw std::runtime_error("missing numeric similarity");
		return r["similarity"].get<double>();
	}
	catch (const std::exception& e)
	{
		LogError("binja_diff_view: invalid rust_diff similarity response: %s", e.what());
		return std::nullopt;
	}
}

}  // namespace bn_graph_diff
