#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace bn_graph_diff {

// Symbol names and token text come from the analysed binary and are not
// guaranteed to be valid UTF-8. nlohmann's default dump() throws type_error.316
// on such bytes, which would fail a whole extraction or request because of one
// malformed name. Serialise with U+FFFD substitution instead, so both sides of
// a comparison see the same stable replacement text.
inline std::string dumpBinaryDerivedJson(const nlohmann::json& value, int indent = -1)
{
	return value.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace bn_graph_diff
