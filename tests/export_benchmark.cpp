// Explicit serialization benchmark. No BinaryView or plugin session is opened.
#include "corpuspersistence.h"
#include <chrono>
#include <iostream>

using namespace bn_graph_diff;

int main(int argc, char** argv)
{
	if (argc != 4)
	{
		std::cerr << "Usage: export_benchmark json|csv OUTPUT FAMILIES\n";
		return 2;
	}
	const bool asJson = std::string(argv[1]) == "json";
	if (!asJson && std::string(argv[1]) != "csv") return 2;
	CorpusManifest manifest;
	manifest.binaries = {{1, "old", "old-identity", 0, 0, false},
	    {2, "new", "new-identity", 0, 0, false}};
	manifest.plan = buildCorpusPlan(CorpusMode::Reference, {1, 2}, 1);
	auto pair = std::make_shared<CorpusPairResult>();
	pair->edge = {1, 2};
	pair->state = CorpusPairState::Complete;
	std::map<CorpusEdgeKey, CorpusPairResultRef> pairs {{{1, 2}, pair}};
	std::vector<FunctionFamily> families;
	const size_t count = std::stoull(argv[3]);
	families.reserve(count);
	for (size_t index = 0; index < count; ++index)
	{
		FunctionFamily family;
		family.id = index;
		family.state = FunctionFamilyState::Changed;
		std::string name = "quoted, \"name\"\n" + std::to_string(index) + std::string(128, 'x');
		family.members[1] = {1, 0x1000 + index * 16, name, 24};
		family.members[2] = {2, 0x2000 + index * 16, name, 24};
		family.acceptedLinks.push_back({family.members[1], family.members[2],
		    0.8, 0.9, "Structural", {1, 2}, "Changed"});
		families.push_back(std::move(family));
	}
	std::string error;
	const auto start = std::chrono::steady_clock::now();
	const bool saved = asJson
	    ? exportCorpusJson(argv[2], manifest, pairs, families, &error)
	    : exportCorpusCsv(argv[2], manifest, pairs, families, &error);
	const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::cout << "families=" << count << " elapsed_seconds=" << elapsed << " saved=" << saved << '\n';
	if (!saved) std::cerr << error << '\n';
	return saved ? 0 : 1;
}
