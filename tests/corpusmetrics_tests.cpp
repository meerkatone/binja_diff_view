#include "corpusengine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace bn_graph_diff;

#define check(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
    std::abort(); } } while (false)

static FunctionSummary function(uint64_t address, size_t instructions, const char* name = "named")
{
    FunctionSummary result;
    result.address = address;
    result.instructionCount = instructions;
    result.name = name;
    return result;
}

static void testFiltering()
{
    FunctionMatchMaps maps;
    FunctionMatchRecord changed;
    changed.a = function(1, 10);
    changed.b = function(2, 20);
    changed.similarity = 0.5;
    changed.confidence = 0.8;
    changed.matchType = "Name";
    FunctionMatchRecord exact;
    exact.a = function(3, 4, "sub_3");
    exact.b = function(4, 4, "sub_4");
    exact.similarity = exact.confidence = 1.0;
    exact.matchType = "Exact";
    exact.contentMatch = "Exact";
    maps.matches = {changed, exact};
    maps.unmatchedA = {function(5, 6)};
    maps.unmatchedB = {function(6, 8)};
    CorpusFunctionFlags source = {{1, {true, false}}};
    CorpusFunctionFlags target = {{6, {false, true}}};

    auto raw = calculateCorpusPairMetricsWithFlags(maps, source, target);
    check(raw.sourceFunctionCount == 3 && raw.targetFunctionCount == 3);
    check(raw.sourceInstructionCount == 20 && raw.targetInstructionCount == 32);
    check(raw.acceptedMatchCount == 2 && raw.exactMatchCount == 1);
    check(raw.sourceOnlyInstructionCount == 6 && raw.targetOnlyInstructionCount == 8);
    check(std::abs(raw.meanSimilarity - 0.75) < 1e-9);

    CorpusMetricFilters filters;
    filters.excludeThunks = true;
    filters.excludeExternalStubs = true;
    auto filtered = calculateCorpusPairMetricsWithFlags(maps, source, target, filters);
    check(filtered.sourceFunctionCount == 2 && filtered.targetFunctionCount == 2);
    check(filtered.sourceInstructionCount == 10 && filtered.targetInstructionCount == 24);
    check(filtered.acceptedMatchCount == 1 && filtered.exactMatchCount == 1);
    check(filtered.sourceOnlyInstructionCount == 6 && filtered.targetOnlyInstructionCount == 20);
    check(filtered.sourceOnlyFunctionCount == 1 && filtered.targetOnlyFunctionCount == 1);

    filters = {};
    filters.minimumInstructionCount = 15;
    filtered = calculateCorpusPairMetricsWithFlags(maps, {}, {}, filters);
    check(filtered.sourceFunctionCount == 0 && filtered.targetFunctionCount == 1);
    check(filtered.acceptedMatchCount == 0 && filtered.targetOnlyInstructionCount == 20);

    filters = {};
    filters.includeExactMatches = false;
    filtered = calculateCorpusPairMetricsWithFlags(maps, {}, {}, filters);
    check(filtered.sourceFunctionCount == 2 && filtered.targetFunctionCount == 2);
    check(filtered.acceptedMatchCount == 1 && filtered.changedMatchCount == 1);
    check(filtered.sourceInstructionCount == 16 && filtered.targetInstructionCount == 28);

    filters = {};
    filters.excludeSmallGeneratedFunctions = true;
    filtered = calculateCorpusPairMetricsWithFlags(maps, {}, {}, filters);
    check(filtered.acceptedMatchCount == 1 && filtered.sourceInstructionCount == 16);
    check(filtered.targetInstructionCount == 28);

    // A filter can also retain only the source of an accepted pair.
    filtered = calculateCorpusPairMetricsWithFlags(maps, {}, {{2, {true, false}}},
        {.excludeThunks = true});
    check(filtered.sourceOnlyInstructionCount == 16 && filtered.targetOnlyInstructionCount == 8);
}

static void testBatchLifetime()
{
    for (bool required : {false, true})
    {
        CorpusMetricFilters filters;
        filters.excludeExternalStubs = required;
        // Size and name filters must not cause symbol enumeration.
        filters.minimumInstructionCount = 5;
        filters.excludeSmallGeneratedFunctions = true;
        CorpusMetricFlagBatch batch(filters);
        int captures = 0;
        for (CorpusBinaryId a = 0; a < 10; ++a)
            for (CorpusBinaryId b = a + 1; b < 10; ++b)
                for (auto id : {a, b})
                {
                    const auto& flags = batch.get(id, [&] {
                        ++captures;
                        return CorpusFunctionFlags{{id, {false, true}}};
                    });
                    check(required ? flags.at(id).externalStub : flags.empty());
                }
        check(captures == (required ? 10 : 0));
        // The next filter application must observe fresh flags.
        CorpusMetricFlagBatch next(filters);
        const auto& flags = next.get(0, [&] {
            ++captures;
            return CorpusFunctionFlags{{0, {true, false}}};
        });
        check(required ? flags.at(0).thunk && !flags.at(0).externalStub : flags.empty());
        check(captures == (required ? 11 : 0));
    }
}

static void testCalleeRenameMap()
{
    FunctionMatchMaps maps;
    FunctionMatchRecord unique, duplicateA, duplicateB;
    unique.a = function(1, 10, "foo");
    unique.b = function(2, 10, "bar");
    duplicateA.a = function(3, 10, "duplicate");
    duplicateA.b = function(4, 10, "one");
    duplicateB.a = function(5, 10, "duplicate");
    duplicateB.b = function(6, 10, "two");
    maps.matches = {unique, duplicateA, duplicateB};
    rebuildCalleeRenameMap(maps);
    check(maps.renameMap.size() == 1 && maps.renameMap.at("foo") == "bar");
    maps.unmatchedB.push_back(function(7, 10, "bar"));
    rebuildCalleeRenameMap(maps);
    check(maps.renameMap.empty());
}

int main()
{
    testFiltering();
    testBatchLifetime();
    testCalleeRenameMap();
    std::puts("corpus metric tests passed");
}
