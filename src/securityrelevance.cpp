#include "patchsignature.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string_view>

namespace bn_graph_diff {

namespace {

std::string lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
	    [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

bool containsAny(const std::string& text,
	std::initializer_list<std::string_view> needles)
{
	return std::any_of(needles.begin(), needles.end(), [&text](std::string_view needle) {
		return text.find(needle) != std::string::npos;
	});
}

void appendUnique(std::vector<std::string>& values, const std::string& value)
{
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.push_back(value);
}

struct SecurityEvidenceLattice
{
	bool guard = false;
	bool memoryOrLifetime = false;
	bool authorization = false;
	bool trustBoundary = false;
	bool numericRisk = false;
	bool securityContext = false;
	bool authentication = false;
	bool protocolState = false;
	bool errorSemantics = false;
	bool identityAuthority = false;
	bool capability = false;
	bool maintenanceContext = false;

	void join(const SecurityEvidenceLattice& other)
	{
		guard |= other.guard;
		memoryOrLifetime |= other.memoryOrLifetime;
		authorization |= other.authorization;
		trustBoundary |= other.trustBoundary;
		numericRisk |= other.numericRisk;
		securityContext |= other.securityContext;
		authentication |= other.authentication;
		protocolState |= other.protocolState;
		errorSemantics |= other.errorSemantics;
		identityAuthority |= other.identityAuthority;
		capability |= other.capability;
		maintenanceContext |= other.maintenanceContext;
	}
};

SecurityEvidenceLattice factsFor(const std::string& text)
{
	SecurityEvidenceLattice facts;
	facts.guard = containsAny(text, {"if (", "if ", "cmp", "check", "valid",
	    "guard", "reject", "fail", "<", ">", "!=", "=="});
	facts.memoryOrLifetime = containsAny(text, {"memcpy", "memmove", "strcpy",
	    "strncpy", "strlcpy", "malloc", "calloc", "realloc", "free(",
	    "release", "retain", "dealloc", "use_after", "buffer"});
	facts.authorization = containsAny(text, {"entitlement", "authoriz", "authenticat",
	    "permission", "privilege", "audit_token", "credential", "sandbox",
	    "code_sign", "signature", "keychain", "access_control"});
	facts.trustBoundary = containsAny(text, {"xpc", "mach_msg", "mig_", "socket",
	    "recv", "deserialize", "unmarshal", "decode", "parse", "request",
	    "message", "client", "ipc"});
	facts.numericRisk = containsAny(text, {"length", " len", "len_", "(len", "[len", "size", "count", "offset",
	    "index", "bounds", "range", "overflow", "underflow", "integer"});
	facts.securityContext = containsAny(text, {"security", "secure", "crypto", "cipher",
	    "encrypt", "decrypt", "certificate", "rsa", "keypair", "keychain",
	    "password", "token", "identity"});
	facts.authentication = containsAny(text, {"auth", "password", "credential", "login",
	    "advertis", "security type", "vnc", "srp", "rsa"});
	facts.protocolState = containsAny(text, {"state", "status", "step", "continu",
	    "transition", "phase", "dispatch", "handle", "mechanism", "advertis",
	    "selected type", "security type"});
	facts.errorSemantics = containsAny(text, {"error", "success", "fail", "reject",
	    "invalid", "denied", "not advertised", "return code", "result"});
	facts.identityAuthority = containsAny(text, {"uid", "gid", "root", "user",
	    "identity", "credential", "privilege", "authority", "sandbox", "entitlement"});
	facts.capability = containsAny(text, {"file", "read", "write", "create", "execute",
	    "command", "launch", "helper", "copy"});
	facts.maintenanceContext = containsAny(text, {"os_log", "logging", "telemetry",
	    "diagnostic", "debug", "trace", "metric", "formatting"});
	return facts;
}

}  // namespace

const char* securityRelevanceStateName(SecurityRelevanceState state)
{
	switch (state)
	{
	case SecurityRelevanceState::Unknown: return "Unknown";
	case SecurityRelevanceState::Possible: return "Possible";
	case SecurityRelevanceState::Strong: return "Strong";
	case SecurityRelevanceState::Conflicting: return "Conflicting";
	}
	return "Unknown";
}

SecurityRelevanceAssessment assessSecurityRelevance(const PatchSignature& signature)
{
	SecurityEvidenceLattice delta;
	SecurityEvidenceLattice vulnerableDelta;
	SecurityEvidenceLattice context;
	context.join(factsFor(lower(signature.vulnerable.name + " " + signature.fixed.name)));

	for (const auto& line : signature.changedLines)
	{
		delta.join(factsFor(lower(line.fixedText)));
		vulnerableDelta.join(factsFor(lower(line.vulnerableText)));
	}
	for (const auto& value : signature.changedTokens)
		if (value.starts_with("fixed:"))
			delta.join(factsFor(lower(value)));
		else if (value.starts_with("vulnerable:"))
			vulnerableDelta.join(factsFor(lower(value)));
	for (const auto& value : signature.changedCallTargets)
		if (value.starts_with("added:"))
			delta.join(factsFor(lower(value)));
		else if (value.starts_with("removed:"))
			vulnerableDelta.join(factsFor(lower(value)));
	for (const auto& value : signature.changedConstants)
		if (value.starts_with("added:"))
			delta.numericRisk = true;
		else if (value.starts_with("removed:"))
			vulnerableDelta.numericRisk = true;
	for (const auto& value : signature.changedStringReferences)
		if (value.starts_with("added:"))
			delta.join(factsFor(lower(value)));
		else if (value.starts_with("removed:"))
			vulnerableDelta.join(factsFor(lower(value)));

	SecurityRelevanceAssessment result;
	int points = 0;
	if (delta.guard)
	{
		points += 2;
		appendUnique(result.evidence, "The fixed patch region adds or changes a guard predicate");
	}
	if (delta.memoryOrLifetime)
	{
		points += 2;
		appendUnique(result.evidence,
		    "The changed region references a memory, allocation, or lifetime operation");
	}
	if (delta.authorization)
	{
		points += 3;
		appendUnique(result.evidence,
		    "The fixed patch region changes authorization or security-control logic");
	}
	if (delta.trustBoundary)
	{
		points += 2;
		appendUnique(result.evidence,
		    "The changed region handles an IPC, parsing, or client-input boundary");
	}
	if (delta.numericRisk)
	{
		points += 1;
		appendUnique(result.evidence,
		    "The fixed patch region changes a length, size, index, or range expression");
	}
	if (context.securityContext)
	{
		points += 1;
		appendUnique(result.evidence,
		    "The function name places the change in security-sensitive code; this is weak evidence");
	}
	if (delta.securityContext)
	{
		points += 2;
		appendUnique(result.evidence,
		    "The changed patch region directly references security-sensitive semantics");
	}
	if (delta.authentication && delta.protocolState)
	{
		points += 3;
		appendUnique(result.evidence,
		    "The patch binds authentication negotiation to protocol state");
	}
	if (delta.errorSemantics)
	{
		points += 1;
		appendUnique(result.evidence,
		    "The patch changes success, failure, or continuation handling");
	}
	if (delta.identityAuthority && delta.capability)
	{
		points += 3;
		appendUnique(result.evidence,
		    "The patch couples identity or authority to a privileged capability");
	}
	if (context.trustBoundary && !delta.trustBoundary)
	{
		points += 1;
		appendUnique(result.evidence,
		    "The function name places the change near an external-input boundary");
	}
	if (delta.maintenanceContext)
		appendUnique(result.counterEvidence,
		    "The changed region contains logging, diagnostics, or telemetry terms");
	const bool removedDangerousOperation = vulnerableDelta.memoryOrLifetime
	    && !delta.memoryOrLifetime;
	if (removedDangerousOperation)
	{
		points += 2;
		appendUnique(result.evidence,
		    "The fixed patch region removes a memory, copy, allocation, or lifetime operation");
	}
	if ((vulnerableDelta.authorization && !delta.authorization)
	    || (vulnerableDelta.guard && !delta.guard))
		appendUnique(result.counterEvidence,
		    "The selected fixed side removes a guard or authorization-related operation");

	const bool semanticSecurityPattern =
	    (delta.authentication && delta.protocolState
	        && (delta.guard || delta.errorSemantics))
	    || (delta.identityAuthority && delta.capability
	        && (delta.trustBoundary || delta.authorization))
	    ||
	    (delta.guard && (delta.memoryOrLifetime || delta.authorization
	        || delta.trustBoundary || delta.numericRisk))
	    || (delta.authorization && delta.trustBoundary);
	const bool possiblePattern = delta.authorization || delta.authentication
	    || delta.protocolState || delta.errorSemantics || delta.identityAuthority
	    || delta.capability || delta.trustBoundary
	    || delta.memoryOrLifetime || removedDangerousOperation
	    || (delta.guard && (delta.numericRisk || context.securityContext
	        || context.trustBoundary));
	result.score = std::clamp(points / 8.0, 0.0, 1.0);
	if (semanticSecurityPattern && points >= 4)
		result.state = SecurityRelevanceState::Strong;
	else if (possiblePattern && points >= 2)
		result.state = SecurityRelevanceState::Possible;
	if (result.state != SecurityRelevanceState::Unknown
	    && !result.counterEvidence.empty())
		result.state = SecurityRelevanceState::Conflicting;
	if (result.state == SecurityRelevanceState::Unknown)
	{
		result.score = std::min(result.score, 0.24);
		if (result.evidence.empty())
			result.evidence.push_back(
			    "No security-relevance pattern was established from the changed patch region");
	}
	return result;
}

}  // namespace bn_graph_diff
