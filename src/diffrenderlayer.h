// Render layer that marks the engine-flagged changed tokens inline (wrapping
// them in «» markers) on lines of changed block pairs. Token types/colors are
// untouched, so interaction and theming behave normally; the markers plus the
// line highlight give token-level emphasis within a changed line.
#pragma once

#include "binaryninjaapi.h"

class DiffRenderLayer : public BinaryNinja::RenderLayer
{
  public:
	DiffRenderLayer();

	static void init();
	void ApplyToFlowGraph(BinaryNinja::Ref<BinaryNinja::FlowGraph> graph) override;
	void ApplyToHighLevelILBody(BinaryNinja::Ref<BinaryNinja::Function> function,
	    std::vector<BinaryNinja::LinearDisassemblyLine>& lines) override;

	void apply(BinaryNinja::Ref<BinaryNinja::BasicBlock> block,
	    std::vector<BinaryNinja::DisassemblyTextLine>& lines);

	void ApplyToDisassemblyBlock(BinaryNinja::Ref<BinaryNinja::BasicBlock> block,
	    std::vector<BinaryNinja::DisassemblyTextLine>& lines) override
	{
		apply(block, lines);
	}
	void ApplyToLowLevelILBlock(BinaryNinja::Ref<BinaryNinja::BasicBlock> block,
	    std::vector<BinaryNinja::DisassemblyTextLine>& lines) override
	{
		apply(block, lines);
	}
	void ApplyToMediumLevelILBlock(BinaryNinja::Ref<BinaryNinja::BasicBlock> block,
	    std::vector<BinaryNinja::DisassemblyTextLine>& lines) override
	{
		apply(block, lines);
	}
	void ApplyToHighLevelILBlock(BinaryNinja::Ref<BinaryNinja::BasicBlock> block,
	    std::vector<BinaryNinja::DisassemblyTextLine>& lines) override
	{
		apply(block, lines);
	}
};
