#include "diffrenderlayer.h"

#include "diffcontroller.h"
#include "tokenemphasis.h"

using namespace BinaryNinja;

DiffRenderLayer::DiffRenderLayer() : RenderLayer("Binja Diff Token Emphasis") {}

void DiffRenderLayer::init()
{
	RenderLayer::Register(new DiffRenderLayer(), EnabledByDefaultRenderLayerDefaultEnableState);
}

void DiffRenderLayer::ApplyToFlowGraph(Ref<FlowGraph> graph)
{
	// First apply the line/token rendering below, then tint graph nodes from the
	// controller's ephemeral overlay model. This does not mutate BN analysis
	// highlights and vanishes as soon as the graph is refreshed after Clear.
	RenderLayer::ApplyToFlowGraph(graph);
	for (const auto& node : graph->GetNodes())
	{
		Ref<BasicBlock> block = node->GetBasicBlock();
		const auto& lines = node->GetLines();
		if (!block || lines.empty())
			continue;
		Ref<Function> func = block->GetFunction();
		if (!func || !func->GetView())
			continue;
		if (auto blockStart = DiffController::instance().overlayBlockStart(
		        func->GetView(), func->GetStart(), lines))
		{
			if (auto color = DiffController::instance().colorForBlock(
			        func->GetView(), func->GetStart(), *blockStart))
				node->SetHighlight(*color);
		}
	}
}

void DiffRenderLayer::ApplyToHighLevelILBody(
    Ref<Function> function, std::vector<LinearDisassemblyLine>& lines)
{
	if (!function || !function->GetView())
		return;
	for (auto& linearLine : lines)
	{
		auto& line = linearLine.contents;
		if (auto color = DiffController::instance().colorForLinearLine(
		        function->GetView(), function->GetStart(), line.addr, line.instrIndex))
			line.highlight = *color;
		if (auto emphasis = DiffController::instance().emphasisForLinearLine(
		        function->GetView(), function->GetStart(), line.addr, line.instrIndex))
			bn_graph_diff::applyTokenEmphasis(line, emphasis->spans);
	}
}

void DiffRenderLayer::apply(Ref<BasicBlock> block, std::vector<DisassemblyTextLine>& lines)
{
	if (!block || lines.empty())
		return;
	Ref<Function> func = block->GetFunction();
	if (!func || !func->GetView())
		return;
	auto blockStart = DiffController::instance().overlayBlockStart(
	    func->GetView(), func->GetStart(), lines);
	if (!blockStart)
		return;
	auto blockColor = DiffController::instance().colorForBlock(
	    func->GetView(), func->GetStart(), *blockStart);
	for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++)
	{
		auto& line = lines[lineIndex];
		if (auto lineColor = DiffController::instance().colorForLine(
		        func->GetView(), func->GetStart(), *blockStart, line.addr, line.instrIndex))
			line.highlight = *lineColor;
		else if (blockColor)
			line.highlight = *blockColor;

		if (auto emphasis = DiffController::instance().emphasisForLine(
		        func->GetView(), func->GetStart(), *blockStart, line.addr, line.instrIndex))
			bn_graph_diff::applyTokenEmphasis(line, emphasis->spans);
	}
}
