#include "jsontext.h"

#include <cstdlib>
#include <iostream>

using namespace bn_graph_diff;
using json = nlohmann::json;

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
	// A Latin-1 byte in a symbol name is not valid UTF-8.
	const std::string malformed = "caf\xe9_handler";
	json lines = json::array({json::array({json::array({"CodeSymbolToken", malformed})})});

	bool strictThrows = false;
	try
	{
		(void)lines.dump();
	}
	catch (const json::type_error&)
	{
		strictThrows = true;
	}
	require(strictThrows, "strict dump no longer rejects invalid UTF-8; revisit jsontext.h");

	std::string dumped;
	try
	{
		dumped = dumpBinaryDerivedJson(lines);
	}
	catch (const std::exception&)
	{
		require(false, "binary-derived JSON dump threw on invalid UTF-8");
	}
	json parsed = json::parse(dumped, nullptr, false);
	require(parsed.is_array(), "replacement dump is not valid JSON");
	require(parsed[0][0][1].get<std::string>() == "caf\xef\xbf\xbd_handler",
	    "invalid byte was not replaced with U+FFFD");
	// The replaced text must survive a second strict serialisation, because
	// stored block lines are parsed and embedded in later engine requests.
	require(parsed.dump() == dumped, "replacement dump did not round-trip");

	// Valid text, including multi-byte names, is left exactly as it was.
	json valid = {{"name", "größe_λ"}, {"count", 3}};
	require(dumpBinaryDerivedJson(valid) == valid.dump(), "valid UTF-8 changed");
	require(dumpBinaryDerivedJson(valid, 2) == valid.dump(2), "indented dump changed");
	return 0;
}
