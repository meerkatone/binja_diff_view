use serde::{Deserialize, Serialize};

/// Generated-name policy shared by matching and similarity scoring. Prefixes
/// only count when followed by a hexadecimal address/id, preserving analyst
/// names such as `sub_parse_header`.
pub(crate) fn is_generated_function_name(name: &str) -> bool {
    if name.is_empty() || name == "unnamed" || name.starts_with("j_") {
        return true;
    }
    let separator = name.find('_').or_else(|| {
        let lower = name.to_ascii_lowercase();
        lower.starts_with("fcn.").then_some(3)
    });
    let Some(separator) = separator else {
        return false;
    };
    let prefix = name[..separator].to_ascii_lowercase();
    if !matches!(
        prefix.as_str(),
        "sub" | "fun" | "func" | "loc" | "nullsub" | "fcn"
    ) {
        return false;
    }
    let suffix = &name[separator + 1..];
    !suffix.is_empty() && suffix.bytes().all(|byte| byte.is_ascii_hexdigit())
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct InstructionInfo {
    pub address: u64,
    pub mnemonic: String,
    pub operands: Vec<String>,
    /// Binary Ninja instruction-text token type for each operand. Older JSON
    /// producers omit this field; unknown operands remain literal.
    pub operand_types: Vec<u8>,
    pub bytes: Vec<u8>,
    pub length: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct BasicBlockInfo {
    pub address: u64,
    pub size: u64,
    pub instructions: Vec<InstructionInfo>,
    pub edges: Vec<u64>,
    pub mnemonic_hash: String,
    pub instruction_count: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct FunctionInfo {
    pub name: String,
    pub address: u64,
    pub size: u64,
    pub basic_blocks: Vec<BasicBlockInfo>,
    pub instructions: Vec<InstructionInfo>,
    pub cyclomatic_complexity: u32,
    pub call_graph_hash: String,
    pub cfg_hash: String,
    pub instruction_count: usize,
    pub call_count: usize,
    /// Addresses of functions this function calls (in its own binary's address space).
    pub callees: Vec<u64>,
    /// Addresses of functions that call this function.
    pub callers: Vec<u64>,
    /// String literals referenced by this function. These are semantic hints
    /// for security triage, never matching keys.
    pub strings: Vec<String>,
    /// Whole-diff security assessment. Inputs omit this field; it is populated
    /// for added/removed functions in the result.
    pub security: SecurityAssessment,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct MatchSuggestion {
    pub a_address: u64,
    pub b_address: u64,
    pub body_similarity: f64,
}

impl FunctionInfo {
    pub(crate) fn estimated_heap_bytes(&self) -> usize {
        fn strings(values: &Vec<String>) -> usize {
            values.capacity() * std::mem::size_of::<String>()
                + values.iter().map(String::capacity).sum::<usize>()
        }
        fn instructions(values: &Vec<InstructionInfo>) -> usize {
            values.capacity() * std::mem::size_of::<InstructionInfo>()
                + values
                    .iter()
                    .map(|i| {
                        i.mnemonic.capacity()
                            + strings(&i.operands)
                            + i.operand_types.capacity()
                            + i.bytes.capacity()
                    })
                    .sum::<usize>()
        }
        self.name.capacity()
            + self.call_graph_hash.capacity()
            + self.cfg_hash.capacity()
            + self.basic_blocks.capacity() * std::mem::size_of::<BasicBlockInfo>()
            + self
                .basic_blocks
                .iter()
                .map(|b| {
                    instructions(&b.instructions)
                        + b.edges.capacity() * std::mem::size_of::<u64>()
                        + b.mnemonic_hash.capacity()
                })
                .sum::<usize>()
            + instructions(&self.instructions)
            + (self.callees.capacity() + self.callers.capacity()) * std::mem::size_of::<u64>()
            + strings(&self.strings)
            + strings(&self.security.evidence)
            + strings(&self.security.counter_evidence)
    }

    /// Clone stable result metadata without cloning instruction payloads used
    /// only while matching. The serialized result is identical to cloning the
    /// complete function and slimming it immediately afterwards.
    pub(crate) fn slimmed_clone(&self) -> Self {
        Self {
            name: self.name.clone(),
            address: self.address,
            size: self.size,
            basic_blocks: self
                .basic_blocks
                .iter()
                .map(|block| BasicBlockInfo {
                    address: block.address,
                    size: block.size,
                    instructions: Vec::new(),
                    edges: block.edges.clone(),
                    mnemonic_hash: block.mnemonic_hash.clone(),
                    instruction_count: block.instruction_count,
                })
                .collect(),
            instructions: Vec::new(),
            cyclomatic_complexity: self.cyclomatic_complexity,
            call_graph_hash: self.call_graph_hash.clone(),
            cfg_hash: self.cfg_hash.clone(),
            instruction_count: self.instruction_count,
            call_count: self.call_count,
            callees: self.callees.clone(),
            callers: self.callers.clone(),
            strings: Vec::new(),
            security: self.security.clone(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct DiffResult {
    pub matched_functions: Vec<FunctionMatch>,
    pub unmatched_functions_a: Vec<FunctionInfo>,
    pub unmatched_functions_b: Vec<FunctionInfo>,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub review_suggestions: Vec<MatchSuggestion>,
    pub similarity_score: f64,
    /// Mean similarity of matched pairs only.
    pub matched_similarity_score: f64,
    /// Fraction of functions covered by a one-to-one match, using the larger
    /// binary's function count as the denominator.
    pub match_coverage: f64,
    pub analysis_time: f64,
    pub binary_a_name: String,
    pub binary_b_name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct FunctionMatch {
    pub function_a: FunctionInfo,
    pub function_b: FunctionInfo,
    pub similarity: f64,
    pub confidence: f64,
    /// Difference between this candidate's pairing score and the strongest
    /// local alternative sharing either endpoint. A zero margin means the
    /// assignment needed global one-to-one constraints to break a tie.
    pub ambiguity_margin: f64,
    pub match_type: MatchType,
    /// Body classification kept separate from the pairing score. A high
    /// similarity can still contain a real patch, while address-only changes
    /// should not appear in the Changed filter.
    pub content_match: ContentMatch,
    pub details: MatchDetails,
    /// Product-lattice assessment of whether the patch is security relevant.
    pub security: SecurityAssessment,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default, PartialEq, Eq)]
pub enum SecurityRelevanceState {
    #[default]
    Unknown,
    Possible,
    Strong,
    Conflicting,
}

/// Public projection of the finite product lattice. Each field is 0 (absent),
/// 1 (context/one-hop), or 2 (direct patch-delta evidence).
#[derive(Debug, Clone, Serialize, Deserialize, Default, PartialEq, Eq)]
#[serde(default)]
pub struct SecurityFacts {
    pub authentication: u8,
    pub protocol_state: u8,
    pub error_semantics: u8,
    pub external_input: u8,
    pub identity_authority: u8,
    pub capability: u8,
    pub memory: u8,
    pub numeric: u8,
    pub guard: u8,
    pub cryptography: u8,
    pub maintenance: u8,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default, PartialEq)]
#[serde(default)]
pub struct SecurityAssessment {
    pub state: SecurityRelevanceState,
    pub score: f64,
    pub direct_score: f64,
    pub evidence: Vec<String>,
    pub counter_evidence: Vec<String>,
    pub propagated: bool,
    pub facts: SecurityFacts,
}

#[derive(Debug, Clone, Hash, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum ContentMatch {
    Exact,
    Cosmetic,
    #[default]
    Changed,
}

#[derive(Debug, Clone, Hash, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum MatchType {
    Exact,
    Name,
    MdIndex,
    SmallPrimes,
    Structural,
    CallGraph,
    #[default]
    Heuristic,
    Manual,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct MatchDetails {
    pub neighbor_support: usize,
    pub neighbor_contradictions: usize,
    pub binding_anchor: bool,
    pub cfg_similarity: f64,
    pub bb_similarity: f64,
    pub instruction_similarity: f64,
    pub edge_similarity: f64,
    pub name_similarity: f64,
    pub call_similarity: f64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn slimmed_clone_matches_post_match_result_shape() {
        let instruction = InstructionInfo {
            address: 0x1000,
            mnemonic: "mov".into(),
            operands: vec!["eax".into(), "1".into()],
            operand_types: vec![3, 4],
            bytes: vec![0xb8, 1, 0, 0, 0],
            length: 5,
        };
        let function = FunctionInfo {
            name: "example".into(),
            address: 0x1000,
            size: 5,
            basic_blocks: vec![BasicBlockInfo {
                address: 0x1000,
                size: 5,
                instructions: vec![instruction.clone()],
                edges: vec![0x1005],
                mnemonic_hash: "hash".into(),
                instruction_count: 1,
            }],
            instructions: vec![instruction],
            instruction_count: 1,
            ..Default::default()
        };
        let mut old_shape = function.clone();
        old_shape.instructions.clear();
        for block in &mut old_shape.basic_blocks {
            block.instructions.clear();
        }
        assert_eq!(
            serde_json::to_value(function.slimmed_clone()).unwrap(),
            serde_json::to_value(old_shape).unwrap()
        );
    }
}
