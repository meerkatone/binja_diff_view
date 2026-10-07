#pragma once

#include "diffengine.h"

namespace bn_graph_diff {

inline void applyTokenEmphasis(BinaryNinja::DisassemblyTextLine& line,
    const std::vector<TokenSpan>& spans)
{
	// ShowAddress prepends an IL index/address column that extraction omits.
	// Preserve that display prefix without consuming engine span positions.
	size_t bodyStart = 0;
	bool sawAddress = false;
	for (size_t i = 0; i < line.tokens.size(); i++)
	{
		const auto type = line.tokens[i].type;
		if (type == AddressDisplayToken)
			sawAddress = true;
		else if (type == AddressSeparatorToken && sawAddress)
		{
			bodyStart = i + 1;
			break;
		}
		else if (type != TextToken && !tokenIgnoredForDiff(type))
			break;
	}

	// Keep positional correspondence so repeated operand text is unambiguous.
	std::vector<BinaryNinja::InstructionTextToken> out;
	out.reserve(line.tokens.size() + 2 * spans.size());
	// With addresses hidden, extraction retains an empty separator token. The
	// visible prefix replaces that token, so also consume its unchanged span.
	size_t spanIndex = bodyStart && !spans.empty() && spans.front().text.empty()
	    && !spans.front().changed ? 1 : 0;
	for (size_t i = 0; i < line.tokens.size(); i++)
	{
		const auto& tok = line.tokens[i];
		// Linear view adds indentation after its address/collapse columns.
		// Consume it only when extraction supplied the same indentation span.
		const bool displayIndentation = tok.type == IndentationToken
		    && (spanIndex >= spans.size() || tok.text != spans[spanIndex].text);
		if (i < bodyStart || tokenIgnoredForDiff(tok.type) || displayIndentation)
		{
			out.push_back(tok);
			continue;
		}
		bool changed = spanIndex < spans.size() && spans[spanIndex].changed
		    && tok.text == spans[spanIndex].text;
		spanIndex++;
		if (changed)
			out.emplace_back(TextToken, "«");
		out.push_back(tok);
		if (changed)
			out.emplace_back(TextToken, "»");
	}
	line.tokens = std::move(out);
}

}  // namespace bn_graph_diff
