#include "tokenemphasis.h"

#include <cstdlib>
#include <iostream>

using namespace BinaryNinja;
using namespace bn_graph_diff;

static std::string text(const DisassemblyTextLine& line)
{
	std::string result;
	for (const auto& token : line.tokens)
		result += token.text;
	return result;
}

static void require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}

int main()
{
	const std::vector<InstructionTextToken> body {
	    {LocalVariableToken, "arg1"}, {OperationToken, " + "},
	    {IntegerToken, "0x18"}, {OperationToken, " + "}, {IntegerToken, "0x18"}};
	const std::vector<TokenSpan> spans {
	    {"arg1", false}, {" + ", false}, {"0x18", false},
	    {" + ", false}, {"0x18", true}};
	DisassemblyTextLine plain;
	plain.tokens = body;
	applyTokenEmphasis(plain, spans);
	require(text(plain) == "arg1 + 0x18 + «0x18»", "wrong repeated operand emphasized");

	DisassemblyTextLine addressed;
	addressed.tokens = {{AddressDisplayToken, "   0"}, {TextToken, " @ "},
	    {AddressDisplayToken, "100043d04"}, {AddressSeparatorToken, "  "}};
	addressed.tokens.insert(addressed.tokens.end(), body.begin(), body.end());
	applyTokenEmphasis(addressed, spans);
	require(text(addressed) == "   0 @ 100043d04  arg1 + 0x18 + «0x18»",
	    "address prefix shifted operand emphasis");

	// Binary Ninja keeps an empty AddressSeparatorToken when ShowAddress is
	// disabled. Extraction sends its empty span; displayed addresses replace it.
	auto extractedSpans = spans;
	extractedSpans.insert(extractedSpans.begin(), {"", false});
	DisassemblyTextLine hiddenAddress;
	hiddenAddress.tokens = {{AddressSeparatorToken, ""}};
	hiddenAddress.tokens.insert(hiddenAddress.tokens.end(), body.begin(), body.end());
	applyTokenEmphasis(hiddenAddress, extractedSpans);
	require(text(hiddenAddress) == "arg1 + 0x18 + «0x18»",
	    "hidden address separator shifted operand emphasis");
	DisassemblyTextLine realAddress;
	realAddress.tokens = {{AddressDisplayToken, "   0"}, {TextToken, " @ "},
	    {AddressDisplayToken, "100043d04"}, {AddressSeparatorToken, "  "}};
	realAddress.tokens.insert(realAddress.tokens.end(), body.begin(), body.end());
	applyTokenEmphasis(realAddress, extractedSpans);
	require(text(realAddress) == "   0 @ 100043d04  arg1 + 0x18 + «0x18»",
	    "extracted empty separator shifted visible-address emphasis");
	DisassemblyTextLine disassemblyAddress;
	disassemblyAddress.tokens = {{AddressDisplayToken, "100043d04"},
	    {AddressSeparatorToken, "  "}};
	disassemblyAddress.tokens.insert(disassemblyAddress.tokens.end(), body.begin(), body.end());
	applyTokenEmphasis(disassemblyAddress, extractedSpans);
	require(text(disassemblyAddress) == "100043d04  arg1 + 0x18 + «0x18»",
	    "disassembly address prefix shifted operand emphasis");
	DisassemblyTextLine linearAddress;
	linearAddress.tokens = {{AddressDisplayToken, "100043d04"},
	    {AddressSeparatorToken, "  "}, {CollapseStateIndicatorToken, "  "},
	    {IndentationToken, "    "}};
	linearAddress.tokens.insert(linearAddress.tokens.end(), body.begin(), body.end());
	applyTokenEmphasis(linearAddress, extractedSpans);
	require(text(linearAddress) == "100043d04        arg1 + 0x18 + «0x18»",
	    "linear indentation shifted operand emphasis");
	DisassemblyTextLine linearHiddenAddress;
	linearHiddenAddress.tokens = {{AddressSeparatorToken, ""},
	    {CollapseStateIndicatorToken, "  "}, {IndentationToken, "    "}};
	linearHiddenAddress.tokens.insert(linearHiddenAddress.tokens.end(), body.begin(), body.end());
	applyTokenEmphasis(linearHiddenAddress, extractedSpans);
	require(text(linearHiddenAddress) == "      arg1 + 0x18 + «0x18»",
	    "hidden-address linear indentation shifted operand emphasis");
	DisassemblyTextLine extractedIndentation;
	extractedIndentation.tokens = {{IndentationToken, "    "}};
	extractedIndentation.tokens.insert(extractedIndentation.tokens.end(), body.begin(), body.end());
	auto indentedSpans = spans;
	indentedSpans.insert(indentedSpans.begin(), {"    ", false});
	applyTokenEmphasis(extractedIndentation, indentedSpans);
	require(text(extractedIndentation) == "    arg1 + 0x18 + «0x18»",
	    "matching extracted indentation was skipped");

	DisassemblyTextLine commented;
	commented.tokens = body;
	commented.tokens.insert(commented.tokens.begin() + 4, {CommentToken, "/*note*/"});
	applyTokenEmphasis(commented, spans);
	require(text(commented) == "arg1 + 0x18 + /*note*/«0x18»",
	    "ignored comment shifted operand emphasis");

	DisassemblyTextLine mismatched;
	mismatched.tokens = body;
	applyTokenEmphasis(mismatched, {{"different", true}});
	require(text(mismatched) == "arg1 + 0x18 + 0x18", "mismatched span marked an operand");
	std::cout << "Token emphasis regressions passed\n";
}
