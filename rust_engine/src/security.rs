//! Security-relevance triage for the whole diff.
//!
//! This is intentionally an evidence lattice, not a vulnerability oracle. It
//! joins independent patch-delta facts, then runs a bounded call-neighborhood
//! fixed point. Only direct evidence propagates, only into another changed/new
//! function with a direct patch mechanic, and propagated evidence cannot
//! propagate again.

use crate::algorithms::{is_call_mnemonic, normalized_instruction_content};
use crate::types::{
    ContentMatch, FunctionInfo, FunctionMatch, SecurityAssessment, SecurityFacts,
    SecurityRelevanceState,
};
use rustc_hash::{FxHashMap, FxHashSet};
use std::collections::BTreeSet;

pub struct DiffSecurityAssessments {
    pub added: FxHashMap<u64, SecurityAssessment>,
    pub removed: FxHashMap<u64, SecurityAssessment>,
}

#[derive(Clone)]
struct Node {
    address: u64,
    facts: SecurityFacts,
    direct_facts: SecurityFacts,
    evidence: BTreeSet<String>,
    counter: BTreeSet<String>,
    neighbors: Vec<usize>,
    propagated: bool,
    match_index: Option<usize>,
}

const AUTH: &[&str] = &[
    "auth",
    "password",
    "credential",
    "login",
    "advertis",
    "selected type",
    "security type",
    "vnc",
    "srp",
    "rsa",
];
const STATE: &[&str] = &[
    "state",
    "status",
    "step",
    "continu",
    "transition",
    "phase",
    "dispatch",
    "handle",
    "mechanism",
    "advertis",
    "selected type",
    "security type",
];
const ERROR: &[&str] = &[
    "error",
    "success",
    "fail",
    "reject",
    "invalid",
    "denied",
    "not advertised",
    "close connection",
    "return code",
    "result",
];
const EXTERNAL: &[&str] = &[
    "client",
    "server",
    "viewer",
    "message",
    "packet",
    "frame",
    "network",
    "socket",
    "recv",
    "request",
    "response",
    "netbuffer",
    "remote",
];
const IDENTITY: &[&str] = &[
    "uid",
    "gid",
    "root",
    "user",
    "identity",
    "credential",
    "privilege",
    "authority",
    "sandbox",
    "audit token",
    "entitlement",
];
const CAPABILITY: &[&str] = &[
    "file", "read", "write", "create", "execute", "command", "launch", "helper", "copy",
];
const MEMORY: &[&str] = &[
    "memcpy", "memmove", "strcpy", "buffer", "alloc", "overflow", "bounds",
];
const NUMERIC: &[&str] = &[
    "length", "size", "count", "offset", "negative", "overflow", "range", "maximum", "minimum",
];
const CRYPTO: &[&str] = &[
    "srp",
    "rsa",
    "crypto",
    "encrypt",
    "decrypt",
    "tls",
    "certificate",
    "key exchange",
];
const MAINTENANCE: &[&str] = &[
    "logging",
    "logger",
    "debug",
    "trace",
    "metric",
    "telemetry",
    "cosmetic",
    "rename",
    "copyright",
    "version string",
];

fn contains_any(text: &str, terms: &[&str]) -> bool {
    terms.iter().any(|term| text.contains(term))
}

fn is_generated_wpp_trace_identifier(value: &str) -> bool {
    let value = value
        .trim_matches(|character: char| !character.is_ascii_alphanumeric() && character != '_');
    let Some(value) = value.strip_prefix("wpp_") else {
        return false;
    };
    let Some(identifier) = value.strip_suffix("_traceguids") else {
        return false;
    };
    identifier.len() == 32 && identifier.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn contains_substantive_maintenance(text: &str) -> bool {
    text.split('\u{1f}')
        .any(|part| !is_generated_wpp_trace_identifier(part) && contains_any(part, MAINTENANCE))
}

// Text/data references are context even when the renderer places them in an
// instruction operand. Only a code symbol used by a call is operation evidence.
fn contextual_operand(instruction: &crate::InstructionInfo, index: usize) -> bool {
    let operand = instruction.operands[index].trim();
    matches!(instruction.operand_types.get(index), Some(17 | 65))
        || (instruction.operand_types.get(index) == Some(&64)
            && !is_call_mnemonic(&instruction.mnemonic.to_ascii_lowercase()))
        || operand.starts_with('"')
        || operand.starts_with("u\"")
}

fn context_strings(function: &FunctionInfo) -> FxHashSet<String> {
    function
        .strings
        .iter()
        .cloned()
        .chain(function.instructions.iter().flat_map(|ins| {
            ins.operands
                .iter()
                .enumerate()
                .filter_map(move |(i, operand)| {
                    contextual_operand(ins, i).then_some(operand.clone())
                })
        }))
        .map(|s| s.to_ascii_lowercase())
        .collect()
}

fn instruction_key(function: &FunctionInfo) -> Vec<String> {
    function
        .instructions
        .iter()
        .map(|instruction| {
            let mut operation = instruction.clone();
            for (i, operand) in operation.operands.iter_mut().enumerate() {
                if contextual_operand(instruction, i) {
                    *operand = "<context>".into();
                }
            }
            normalized_instruction_content(&operation)
        })
        .collect()
}

fn multiset_added(before: &[String], after: &[String]) -> Vec<String> {
    let mut remaining: FxHashMap<&str, usize> = FxHashMap::default();
    for item in before {
        *remaining.entry(item.as_str()).or_default() += 1;
    }
    let mut added = Vec::new();
    for item in after {
        let count = remaining.entry(item.as_str()).or_default();
        if *count > 0 {
            *count -= 1;
        } else {
            added.push(item.clone());
        }
    }
    added
}

fn join_fact(slot: &mut u8, level: u8) -> bool {
    if level > *slot {
        *slot = level;
        true
    } else {
        false
    }
}

fn observe(facts: &mut SecurityFacts, evidence: &mut BTreeSet<String>, text: &str, level: u8) {
    let text = text.to_ascii_lowercase();
    let mut hit = |slot: &mut u8, terms: &[&str], label: &str| {
        if contains_any(&text, terms) {
            join_fact(slot, level);
            evidence.insert(label.to_string());
        }
    };
    hit(
        &mut facts.authentication,
        AUTH,
        "authentication negotiation",
    );
    hit(
        &mut facts.protocol_state,
        STATE,
        "protocol/state transition",
    );
    hit(&mut facts.error_semantics, ERROR, "success/error semantics");
    hit(
        &mut facts.external_input,
        EXTERNAL,
        "remote-input trust boundary",
    );
    hit(
        &mut facts.identity_authority,
        IDENTITY,
        "identity or process authority",
    );
    hit(&mut facts.capability, CAPABILITY, "privileged capability");
    hit(&mut facts.memory, MEMORY, "memory/buffer operation");
    hit(&mut facts.numeric, NUMERIC, "length/range arithmetic");
    hit(
        &mut facts.cryptography,
        CRYPTO,
        "cryptographic/authentication mechanism",
    );
    if contains_substantive_maintenance(&text) {
        join_fact(&mut facts.maintenance, level);
        evidence.insert("maintenance-only vocabulary".to_string());
    }
}

fn observe_instruction(facts: &mut SecurityFacts, evidence: &mut BTreeSet<String>, text: &str) {
    observe(facts, evidence, text, 2);
    let mnemonic = text.split('\u{1f}').next().unwrap_or_default();
    if mnemonic == "cmp"
        || mnemonic == "test"
        || matches!(mnemonic, "cbz" | "cbnz" | "tbz" | "tbnz")
        || (mnemonic.starts_with("b."))
        || (mnemonic.starts_with('j') && mnemonic != "jmp")
    {
        join_fact(&mut facts.guard, 2);
        evidence.insert("validation/control-flow instruction".into());
    }
}

fn direct_node(
    before: Option<&FunctionInfo>,
    after: &FunctionInfo,
    match_index: Option<usize>,
) -> Node {
    let before_instructions = before.map(instruction_key).unwrap_or_default();
    let after_instructions = instruction_key(after);
    let added_instructions = multiset_added(&before_instructions, &after_instructions);
    let removed_instructions = multiset_added(&after_instructions, &before_instructions);
    let before_strings = before.map(context_strings).unwrap_or_default();
    let after_strings = context_strings(after);
    let mut added_strings: Vec<_> = after_strings.difference(&before_strings).cloned().collect();
    added_strings.sort();

    let mut facts = SecurityFacts::default();
    let mut evidence = BTreeSet::new();
    for (direction, instructions) in [
        ("added", &added_instructions),
        ("removed", &removed_instructions),
    ] {
        for instruction in instructions {
            let mut labels = BTreeSet::new();
            observe_instruction(&mut facts, &mut labels, instruction);
            evidence.extend(
                labels
                    .into_iter()
                    .map(|label| format!("{direction}: {label}")),
            );
        }
    }
    let direct_facts = facts.clone();
    // Strings describe context, not verified operations or trust boundaries.
    for string in &added_strings {
        let mut labels = BTreeSet::new();
        observe(&mut facts, &mut labels, string, 1);
        evidence.extend(
            labels
                .into_iter()
                .map(|label| format!("added string context: {label}")),
        );
    }
    for string in before_strings.difference(&after_strings) {
        let mut labels = BTreeSet::new();
        observe(&mut facts, &mut labels, string, 1);
        evidence.extend(
            labels
                .into_iter()
                .map(|label| format!("removed string context: {label}")),
        );
    }
    // A meaningful name provides context, but can never be direct evidence.
    observe(&mut facts, &mut evidence, &after.name, 1);
    let mut counter = BTreeSet::new();
    if facts.maintenance == 2 {
        counter.insert("delta also resembles logging/maintenance work".into());
    }
    if added_instructions.is_empty()
        && removed_instructions.is_empty()
        && added_strings.is_empty()
        && before_strings.is_subset(&after_strings)
    {
        counter.insert("no semantic delta evidence was extracted".into());
    }
    Node {
        address: after.address,
        facts,
        direct_facts,
        evidence,
        counter,
        neighbors: Vec::new(),
        propagated: false,
        match_index,
    }
}

fn direct_mechanic_count(facts: &SecurityFacts) -> usize {
    [
        facts.guard,
        facts.protocol_state,
        facts.error_semantics,
        facts.memory,
        facts.numeric,
        facts.capability,
    ]
    .into_iter()
    .filter(|value| *value == 2)
    .count()
}

fn transfer(source: &SecurityFacts, target: &mut SecurityFacts) -> bool {
    let mut changed = false;
    for (from, to) in [
        (source.authentication, &mut target.authentication),
        (source.protocol_state, &mut target.protocol_state),
        (source.error_semantics, &mut target.error_semantics),
        (source.external_input, &mut target.external_input),
        (source.identity_authority, &mut target.identity_authority),
        (source.capability, &mut target.capability),
        (source.memory, &mut target.memory),
        (source.numeric, &mut target.numeric),
        (source.cryptography, &mut target.cryptography),
    ] {
        if from == 2 {
            changed |= join_fact(to, 1);
        }
    }
    changed
}

fn score(
    facts: &SecurityFacts,
    direct: &SecurityFacts,
    allow_propagated_pattern: bool,
) -> (SecurityRelevanceState, f64) {
    let present = |value: u8| value > 0;
    let auth_state = present(facts.authentication)
        && present(facts.protocol_state)
        && (present(facts.guard) || present(facts.error_semantics));
    let authority_capability = present(facts.identity_authority)
        && present(facts.capability)
        && present(facts.external_input);
    let memory_boundary = present(facts.memory)
        && present(facts.numeric)
        && present(facts.guard)
        && present(facts.external_input);
    let direct_auth_state =
        direct.authentication == 2 || direct.protocol_state == 2 || direct.error_semantics == 2;
    let direct_authority =
        direct.identity_authority == 2 || direct.capability == 2 || direct.external_input == 2;
    let direct_memory = direct.memory == 2 || direct.numeric == 2 || direct.external_input == 2;
    let strong_auth_state = auth_state && direct_auth_state;
    let strong_authority = authority_capability && direct_authority;
    let strong_memory = memory_boundary && direct_memory;
    let mut value: f64 = if strong_auth_state {
        0.92
    } else if strong_authority {
        0.90
    } else if strong_memory {
        0.87
    } else if allow_propagated_pattern && (auth_state || authority_capability || memory_boundary) {
        0.58
    } else {
        0.0
    };
    if value == 0.0 {
        let dimensions = [
            facts.authentication,
            facts.protocol_state,
            facts.error_semantics,
            facts.external_input,
            facts.identity_authority,
            facts.capability,
            facts.memory,
            facts.numeric,
            facts.guard,
            facts.cryptography,
        ];
        let direct = dimensions.iter().filter(|value| **value == 2).count() as f64;
        let context = dimensions.iter().filter(|value| **value == 1).count() as f64;
        value = (direct * 0.13 + context * 0.055).min(0.79);
    }
    let positive = value >= 0.30;
    let conflicting = positive && facts.maintenance == 2;
    let state = if conflicting {
        SecurityRelevanceState::Conflicting
    } else if strong_auth_state || strong_authority || strong_memory {
        SecurityRelevanceState::Strong
    } else if positive {
        SecurityRelevanceState::Possible
    } else {
        SecurityRelevanceState::Unknown
    };
    (state, value)
}

fn assessment(node: &Node) -> SecurityAssessment {
    let (state, score_value) = score(&node.facts, &node.direct_facts, node.propagated);
    let (_, direct_score) = score(&node.direct_facts, &node.direct_facts, false);
    let mut evidence: Vec<_> = node.evidence.iter().cloned().collect();
    if node.propagated {
        evidence.push("security context joined from a directly connected changed function".into());
    }
    SecurityAssessment {
        state,
        score: score_value,
        direct_score,
        evidence,
        counter_evidence: node.counter.iter().cloned().collect(),
        propagated: node.propagated,
        facts: node.facts.clone(),
    }
}

pub fn assess_diff(
    functions_a: &[FunctionInfo],
    functions_b: &[FunctionInfo],
    matches: &mut [FunctionMatch],
) -> DiffSecurityAssessments {
    let by_a: FxHashMap<_, _> = functions_a
        .iter()
        .map(|function| (function.address, function))
        .collect();
    let by_b: FxHashMap<_, _> = functions_b
        .iter()
        .map(|function| (function.address, function))
        .collect();
    let matched_a: FxHashSet<_> = matches.iter().map(|item| item.function_a.address).collect();
    let matched_b: FxHashSet<_> = matches.iter().map(|item| item.function_b.address).collect();
    let mut nodes = Vec::new();
    for (index, item) in matches.iter().enumerate() {
        if item.content_match != ContentMatch::Changed {
            continue;
        }
        if let (Some(before), Some(after)) = (
            by_a.get(&item.function_a.address),
            by_b.get(&item.function_b.address),
        ) {
            nodes.push(direct_node(Some(before), after, Some(index)));
        }
    }
    for function in functions_b {
        if !matched_b.contains(&function.address) {
            nodes.push(direct_node(None, function, None));
        }
    }

    let node_by_address: FxHashMap<_, _> = nodes
        .iter()
        .enumerate()
        .map(|(index, node)| (node.address, index))
        .collect();
    for node in &mut nodes {
        if let Some(function) = by_b.get(&node.address) {
            let mut neighbors: Vec<_> = function
                .callees
                .iter()
                .chain(function.callers.iter())
                .filter_map(|address| node_by_address.get(address).copied())
                .collect();
            neighbors.sort_unstable();
            neighbors.dedup();
            node.neighbors = neighbors;
        }
    }

    // Level-1 contextual facts never retransmit. One pass over immutable
    // direct facts is the complete evidence-gated one-hop closure.
    let direct: Vec<_> = nodes.iter().map(|node| node.direct_facts.clone()).collect();
    for node in &mut nodes {
        if direct_mechanic_count(&node.direct_facts) < 2 {
            continue;
        }
        for &neighbor in &node.neighbors {
            node.propagated |= transfer(&direct[neighbor], &mut node.facts);
        }
    }

    let mut added = FxHashMap::default();
    for node in &nodes {
        let mut value = assessment(node);
        if let Some(index) = node.match_index {
            if !matches[index].details.binding_anchor && matches[index].ambiguity_margin < 0.01 {
                if value.state == SecurityRelevanceState::Strong {
                    value.state = SecurityRelevanceState::Possible;
                    value.score = value.score.min(0.79);
                    value.direct_score = value.direct_score.min(0.79);
                }
                value.counter_evidence.push(
                    "ambiguous function pairing; verify counterparts before interpreting the delta"
                        .into(),
                );
            }
            matches[index].security = value;
        } else {
            added.insert(node.address, value);
        }
    }

    let mut removed = FxHashMap::default();
    for function in functions_a {
        if matched_a.contains(&function.address) {
            continue;
        }
        let empty = FunctionInfo {
            address: function.address,
            name: function.name.clone(),
            ..Default::default()
        };
        let node = direct_node(Some(function), &empty, None);
        let mut value = assessment(&node);
        if value.state != SecurityRelevanceState::Unknown {
            value.state = SecurityRelevanceState::Conflicting;
            value.counter_evidence.push(
                "function was removed; the direction of the security effect is uncertain".into(),
            );
        }
        removed.insert(function.address, value);
    }
    DiffSecurityAssessments { added, removed }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{InstructionInfo, MatchDetails, MatchType};

    fn function(
        address: u64,
        name: &str,
        instructions: &[(&str, &str)],
        strings: &[&str],
    ) -> FunctionInfo {
        FunctionInfo {
            address,
            name: name.into(),
            instructions: instructions
                .iter()
                .map(|(mnemonic, operands)| InstructionInfo {
                    mnemonic: (*mnemonic).into(),
                    operands: vec![(*operands).into()],
                    ..Default::default()
                })
                .collect(),
            strings: strings.iter().map(|value| (*value).into()).collect(),
            ..Default::default()
        }
    }

    fn changed(before: &FunctionInfo, after: &FunctionInfo) -> FunctionMatch {
        FunctionMatch {
            function_a: before.slimmed_clone(),
            function_b: after.slimmed_clone(),
            content_match: ContentMatch::Changed,
            match_type: MatchType::Heuristic,
            ambiguity_margin: 1.0,
            details: MatchDetails::default(),
            ..Default::default()
        }
    }

    #[test]
    fn operand_context_cannot_masquerade_as_direct_operations() {
        for kind in [17, 65, 64] {
            let mut before = function(1, "worker", &[("mov", "old message")], &[]);
            before.instructions[0].operand_types = vec![kind];
            let mut after = before.clone();
            after.address = 2;
            after.instructions[0].operands[0] = "remote user root file write helper".into();
            let mut matches = vec![changed(&before, &after)];
            assess_diff(&[before], &[after], &mut matches);
            assert_eq!(matches[0].security.direct_score, 0.0, "token {kind}");
            assert_ne!(matches[0].security.state, SecurityRelevanceState::Strong);
        }
        // An executed call to a named capability remains operation evidence.
        let before = function(1, "worker", &[("ret", "")], &[]);
        let mut after = function(2, "worker", &[("bl", "create_file")], &[]);
        after.instructions[0].operand_types = vec![64];
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);
        assert_eq!(matches[0].security.facts.capability, 2);
    }

    #[test]
    fn guard_detection_recognizes_x86_conditions_not_arm_bitwise_or_unconditional_branches() {
        for mnemonic in ["jne", "jae", "b.eq", "cbz", "test"] {
            let mut facts = SecurityFacts::default();
            observe_instruction(&mut facts, &mut BTreeSet::new(), mnemonic);
            assert_eq!(facts.guard, 2, "{mnemonic}");
        }
        for mnemonic in ["b", "br", "bl", "blr", "bic", "bfi", "jmp"] {
            let mut facts = SecurityFacts::default();
            observe_instruction(&mut facts, &mut BTreeSet::new(), mnemonic);
            assert_eq!(facts.guard, 0, "{mnemonic}");
        }
    }

    #[test]
    fn removal_only_patch_retains_directional_security_evidence() {
        let before = function(
            1,
            "worker",
            &[
                ("cmp", "auth_state"),
                ("bl", "reject_invalid_auth"),
                ("ret", ""),
            ],
            &[],
        );
        let after = function(2, "worker", &[("ret", "")], &[]);
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);
        let security = &matches[0].security;
        assert_eq!(security.state, SecurityRelevanceState::Strong);
        assert!(security.evidence.iter().any(|s| s.starts_with("removed:")));
        assert!(!security.evidence.iter().any(|s| s.starts_with("added:")));
        assert!(!security
            .counter_evidence
            .iter()
            .any(|s| s.contains("no semantic delta")));
    }

    #[test]
    fn whole_removed_function_labels_evidence_as_removed() {
        let before = function(
            1,
            "worker",
            &[("cmp", "auth_state"), ("bl", "reject_invalid_auth")],
            &[],
        );
        let results = assess_diff(&[before], &[], &mut []);
        let security = &results.removed[&1];
        assert_eq!(security.state, SecurityRelevanceState::Conflicting);
        assert!(security.evidence.iter().any(|s| s.starts_with("removed:")));
        assert!(!security.evidence.iter().any(|s| s.starts_with("added:")));
    }

    #[test]
    fn ambiguous_counterparts_do_not_claim_strong_security() {
        let before = function(1, "worker", &[("ret", "")], &[]);
        let after = function(
            2,
            "worker",
            &[("cmp", "auth_state"), ("bl", "reject_invalid_auth")],
            &[],
        );
        let mut pair = changed(&before, &after);
        pair.ambiguity_margin = 0.0;
        let mut matches = vec![pair];
        assess_diff(&[before], &[after], &mut matches);
        assert_eq!(matches[0].security.state, SecurityRelevanceState::Possible);
        assert!(matches[0]
            .security
            .counter_evidence
            .iter()
            .any(|s| s.contains("ambiguous")));
    }

    #[test]
    fn strings_alone_cannot_claim_strong_or_direct_operation_evidence() {
        let before = function(1, "worker", &[("ret", "")], &[]);
        let after = function(
            2,
            "worker",
            &[("ret", "")],
            &["remote user requested root file write helper; authentication state error"],
        );
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);
        let security = &matches[0].security;
        assert_ne!(security.state, SecurityRelevanceState::Strong);
        assert_eq!(security.direct_score, 0.0);
    }

    #[test]
    fn removed_string_is_context_not_an_operation() {
        let before = function(1, "worker", &[("ret", "")], &["authentication state error"]);
        let after = function(2, "worker", &[("ret", "")], &[]);
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);
        assert_eq!(matches[0].security.direct_score, 0.0);
        assert!(matches[0]
            .security
            .evidence
            .iter()
            .any(|s| s.starts_with("removed string context:")));
        assert!(matches[0].security.counter_evidence.is_empty());
    }

    #[test]
    fn authentication_state_guard_is_strong() {
        let before = function(1, "HandleAuthTypeMessage", &[("mov", "x0")], &[]);
        let after = function(
            2,
            "HandleAuthTypeMessage",
            &[("mov", "x0"), ("cmp", "auth_state")],
            &["selected authentication type was not advertised by server"],
        );
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);
        assert_eq!(matches[0].security.state, SecurityRelevanceState::Strong);
        assert!(matches[0].security.score >= 0.9);
    }

    #[test]
    fn authority_capability_is_strong() {
        let before = function(1, "copy_file", &[("mov", "x0")], &[]);
        let after = function(
            2,
            "copy_file",
            &[("mov", "x0"), ("bl", "create_file")],
            &["remote user requested root file write helper"],
        );
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);
        assert_eq!(matches[0].security.state, SecurityRelevanceState::Strong);
    }

    #[test]
    fn maintenance_only_delta_is_not_promoted() {
        let before = function(1, "worker", &[("mov", "x0")], &[]);
        let after = function(
            2,
            "worker",
            &[("mov", "x0"), ("bl", "logger")],
            &["debug logging metric"],
        );
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);
        assert_eq!(matches[0].security.state, SecurityRelevanceState::Unknown);
    }

    #[test]
    fn generated_wpp_trace_guid_churn_does_not_veto_security_evidence() {
        let before = function(
            1,
            "Dot11Translate80211ToEthernetNdisPacket",
            &[
                ("mov", "x0"),
                ("lea", "WPP_d606735c9a8dbdba9f6da3deef1e980d_Traceguids"),
            ],
            &[],
        );
        let after = function(
            2,
            "Dot11Translate80211ToEthernetNdisPacket",
            &[
                ("mov", "x0"),
                ("lea", "WPP_9907afff6807279fc60edbc71b8dfc63_Traceguids"),
                ("cmp", "protocol_state"),
            ],
            &[],
        );
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);

        assert_eq!(matches[0].security.state, SecurityRelevanceState::Possible);
        assert!(!matches[0]
            .security
            .evidence
            .iter()
            .any(|value| value.ends_with("maintenance-only vocabulary")));
        assert!(!matches[0]
            .security
            .counter_evidence
            .iter()
            .any(|value| value.contains("maintenance")));
    }

    #[test]
    fn ordinary_trace_delta_still_counts_as_maintenance() {
        let before = function(1, "worker", &[("mov", "x0")], &[]);
        let after = function(
            2,
            "worker",
            &[("mov", "x0"), ("bl", "TraceSecurityState")],
            &[],
        );
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);

        assert!(matches[0]
            .security
            .evidence
            .iter()
            .any(|value| value.ends_with("maintenance-only vocabulary")));
    }

    #[test]
    fn security_name_and_guard_without_semantic_delta_is_unknown() {
        let before = function(1, "HandleAuthTypeMessage", &[("mov", "x0")], &[]);
        let after = function(
            2,
            "HandleAuthTypeMessage",
            &[("mov", "x0"), ("cbz", "x0")],
            &[],
        );
        let mut matches = vec![changed(&before, &after)];
        assess_diff(&[before], &[after], &mut matches);
        assert_eq!(matches[0].security.state, SecurityRelevanceState::Unknown);
    }

    #[test]
    fn fixed_point_lifts_one_hop_guard_but_not_two_hops() {
        let old_seed = function(1, "auth_helper", &[("mov", "x0")], &[]);
        let mut seed = function(
            11,
            "auth_helper",
            &[("mov", "x0"), ("cmp", "auth_state")],
            &["remote authentication state rejected"],
        );
        let old_caller = function(2, "caller", &[("mov", "x0")], &[]);
        let mut caller = function(
            12,
            "caller",
            &[("mov", "x0"), ("cbz", "x0"), ("bl", "create_file")],
            &[],
        );
        let old_outer = function(3, "outer", &[("mov", "x0")], &[]);
        let mut outer = function(13, "outer", &[("mov", "x0"), ("cbz", "x0")], &[]);
        seed.callers = vec![12];
        caller.callees = vec![11];
        caller.callers = vec![13];
        outer.callees = vec![12];
        let mut matches = vec![
            changed(&old_seed, &seed),
            changed(&old_caller, &caller),
            changed(&old_outer, &outer),
        ];
        assess_diff(
            &[old_seed, old_caller, old_outer],
            &[seed, caller, outer],
            &mut matches,
        );
        assert!(matches[1].security.propagated);
        assert_eq!(matches[1].security.state, SecurityRelevanceState::Possible);
        assert!(!matches[2].security.propagated);
        assert_eq!(matches[2].security.state, SecurityRelevanceState::Unknown);
    }

    #[test]
    fn lone_guard_does_not_inherit_security_context() {
        let old_seed = function(1, "auth_helper", &[("mov", "x0")], &[]);
        let mut seed = function(
            11,
            "auth_helper",
            &[("mov", "x0"), ("cmp", "auth_state")],
            &["remote authentication state rejected"],
        );
        let old_caller = function(2, "generated_helper", &[("mov", "x0")], &[]);
        let mut caller = function(12, "generated_helper", &[("mov", "x0"), ("cbz", "x0")], &[]);
        seed.callers = vec![12];
        caller.callees = vec![11];
        let mut matches = vec![changed(&old_seed, &seed), changed(&old_caller, &caller)];

        assess_diff(&[old_seed, old_caller], &[seed, caller], &mut matches);

        assert!(!matches[1].security.propagated);
        assert_eq!(matches[1].security.state, SecurityRelevanceState::Unknown);
    }
}
