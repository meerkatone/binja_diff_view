use crate::similarity::SimilarityAnalyzer;
use crate::types::{
    is_generated_function_name, ContentMatch, FunctionInfo, MatchDetails, MatchType,
};
use rustc_hash::{FxHashMap, FxHashSet, FxHasher};
use sha2::{Digest, Sha256};
use std::hash::{Hash, Hasher};

pub struct DiffAlgorithms;

/// Immutable features reused while a function participates in many candidate
/// comparisons. Building these histograms and sets once avoids repeatedly
/// walking the same instruction and block vectors in every matching phase.
pub(crate) struct FunctionFeatures {
    bb_mnemonic_counts: FxHashMap<String, usize>,
    instruction_content_counts: FxHashMap<String, usize>,
    instruction_pairs: FxHashMap<(u64, u64), usize>,
    instruction_sequence: Vec<u64>,
    topology_nodes: FxHashMap<u64, usize>,
    topology_edges: FxHashMap<(u64, u64), usize>,
    cfg_node_count: usize,
    cfg_internal_edge_count: usize,
    constants: FxHashSet<String>,
    strings: FxHashSet<String>,
}

impl FunctionFeatures {
    pub(crate) fn retrieval_strings(&self) -> &FxHashSet<String> {
        &self.strings
    }

    pub(crate) fn estimated_heap_bytes(&self) -> usize {
        // Hash table capacity includes an allowance for control bytes/alignment.
        fn table<K, V>(map: &FxHashMap<K, V>) -> usize {
            map.capacity() * (std::mem::size_of::<(K, V)>() + 16)
        }
        table(&self.bb_mnemonic_counts)
            + self
                .bb_mnemonic_counts
                .keys()
                .map(String::capacity)
                .sum::<usize>()
            + table(&self.instruction_content_counts)
            + self
                .instruction_content_counts
                .keys()
                .map(String::capacity)
                .sum::<usize>()
            + table(&self.instruction_pairs)
            + self.instruction_sequence.capacity() * std::mem::size_of::<u64>()
            + table(&self.topology_nodes)
            + table(&self.topology_edges)
            + (self.constants.capacity() + self.strings.capacity())
                * (std::mem::size_of::<String>() + 16)
            + self
                .constants
                .iter()
                .chain(self.strings.iter())
                .map(String::capacity)
                .sum::<usize>()
    }

    fn structural_similarity(&self, other: &Self) -> f64 {
        let count_similarity = |a: usize, b: usize, empty_value: f64| {
            if a == 0 && b == 0 {
                empty_value
            } else if a == 0 || b == 0 {
                0.0
            } else {
                1.0 - (a as f64 - b as f64).abs() / a.max(b) as f64
            }
        };
        let counts = 0.6 * count_similarity(self.cfg_node_count, other.cfg_node_count, 1.0)
            + 0.4
                * count_similarity(
                    self.cfg_internal_edge_count,
                    other.cfg_internal_edge_count,
                    1.0,
                );
        0.5 * counts + 0.5 * histogram_overlap(&self.topology_nodes, &other.topology_nodes)
    }
}

fn histogram_overlap<K: Eq + Hash>(a: &FxHashMap<K, usize>, b: &FxHashMap<K, usize>) -> f64 {
    let total = a.values().sum::<usize>().max(b.values().sum());
    if total == 0 {
        return 0.0;
    }
    a.iter()
        .map(|(key, n)| n.min(b.get(key).unwrap_or(&0)))
        .sum::<usize>() as f64
        / total as f64
}

// Topological labels are independent of instruction text and address ordering.
// Histogram overlap is a structural proxy, not an established node mapping.
fn topology_histograms(
    func: &FunctionInfo,
) -> (FxHashMap<u64, usize>, FxHashMap<(u64, u64), usize>) {
    let succ = internal_successors(func);
    let mut pred = vec![Vec::new(); succ.len()];
    for (a, bs) in succ.iter().enumerate() {
        for &b in bs {
            pred[b].push(a);
        }
    }
    let mut labels: Vec<_> = (0..succ.len())
        .map(|i| stable_hash(&(pred[i].len(), succ[i].len())))
        .collect();
    for _ in 0..2 {
        labels = (0..succ.len())
            .map(|i| {
                let mut before: Vec<_> = pred[i].iter().map(|&j| labels[j]).collect();
                let mut after: Vec<_> = succ[i].iter().map(|&j| labels[j]).collect();
                before.sort_unstable();
                after.sort_unstable();
                stable_hash(&(labels[i], before, after))
            })
            .collect();
    }
    let (mut nodes, mut edges) = (FxHashMap::default(), FxHashMap::default());
    for (a, bs) in succ.iter().enumerate() {
        *nodes.entry(labels[a]).or_insert(0) += 1;
        for &b in bs {
            *edges.entry((labels[a], labels[b])).or_insert(0) += 1;
        }
    }
    (nodes, edges)
}

/// Clamp a score to [0.0, 1.0] and replace NaN with 0.0.
#[inline]
fn sanitize_score(x: f64) -> f64 {
    if x.is_nan() {
        0.0
    } else {
        x.clamp(0.0, 1.0)
    }
}

/// Deterministic (unseeded) hash of any hashable value.
#[inline]
fn stable_hash<T: Hash>(value: &T) -> u64 {
    let mut hasher = FxHasher::default();
    value.hash(&mut hasher);
    hasher.finish()
}

fn normalized_mnemonic(mnemonic: &str) -> String {
    mnemonic.trim().to_ascii_lowercase()
}

fn has_generated_hex_name(name: &str, prefixes: &[&str]) -> bool {
    prefixes.iter().any(|prefix| {
        name.strip_prefix(prefix).is_some_and(|suffix| {
            !suffix.is_empty() && suffix.bytes().all(|byte| byte.is_ascii_hexdigit())
        })
    })
}

/// Binary Ninja renders unnamed code and data references as symbols whose
/// names contain their address. Treat only those generated forms as addresses;
/// imported, debug, and analyst-provided symbol names remain matching evidence.
fn is_generated_address_symbol(operand: &str, token_type: Option<u8>) -> bool {
    match token_type {
        // CodeSymbolToken
        Some(64) => has_generated_hex_name(
            operand,
            &["sub_", "fun_", "func_", "loc_", "nullsub_", "fcn."],
        ),
        // DataSymbolToken
        Some(65) => has_generated_hex_name(
            operand,
            &[
                "data_", "byte_", "word_", "dword_", "qword_", "oword_", "xmmword_", "ymmword_",
                "zmmword_", "unk_", "off_",
            ],
        ),
        _ => false,
    }
}

/// Normalize operands Binary Ninja explicitly classified as addresses without
/// guessing from magnitude. A large IntegerToken may be a security-relevant
/// bound or mask and therefore remains literal.
fn normalized_operand(operand: &str, token_type: Option<u8>) -> String {
    let normalized = operand.trim().to_ascii_lowercase();
    // BNInstructionTextTokenType values for PossibleAddressToken,
    // CodeRelativeAddressToken, and analysis-generated AddressDisplayToken.
    if matches!(token_type, Some(5 | 10 | 68))
        || is_generated_address_symbol(&normalized, token_type)
    {
        return "<address>".to_string();
    }
    normalized
}

pub(crate) fn normalized_instruction_content(instruction: &crate::InstructionInfo) -> String {
    let mut key = normalized_mnemonic(&instruction.mnemonic);
    for (index, operand) in instruction.operands.iter().enumerate() {
        key.push('\u{1f}');
        key.push_str(&normalized_operand(
            operand,
            instruction.operand_types.get(index).copied(),
        ));
    }
    key
}

fn raw_instruction_content(instruction: &crate::InstructionInfo) -> String {
    let mut key = normalized_mnemonic(&instruction.mnemonic);
    for operand in &instruction.operands {
        key.push('\u{1f}');
        key.push_str(
            &operand
                .split_whitespace()
                .collect::<String>()
                .to_ascii_lowercase(),
        );
    }
    key
}

fn internal_successors(func: &FunctionInfo) -> Vec<Vec<usize>> {
    let indices: FxHashMap<u64, usize> = func
        .basic_blocks
        .iter()
        .enumerate()
        .map(|(index, block)| (block.address, index))
        .collect();
    func.basic_blocks
        .iter()
        .map(|block| {
            let mut successors: Vec<usize> = block
                .edges
                .iter()
                .filter_map(|target| indices.get(target).copied())
                .collect();
            successors.sort_unstable();
            successors
        })
        .collect()
}

/// Classify a matched pair independently of its pairing score. Incoming calls
/// are intentionally excluded: adding a call changes the caller, not the
/// callee's body. Outgoing-call count remains a conservative guard when a
/// rendered direct target is only available as a relocatable address.
pub fn classify_content_match(a: &FunctionInfo, b: &FunctionInfo) -> ContentMatch {
    let structure_equal =
        internal_successors(a) == internal_successors(b) && a.callees.len() == b.callees.len();
    if !structure_equal || a.basic_blocks.len() != b.basic_blocks.len() {
        return ContentMatch::Changed;
    }

    let same_sequence = |canonical: bool| {
        a.basic_blocks
            .iter()
            .zip(&b.basic_blocks)
            .all(|(left, right)| {
                left.instructions.len() == right.instructions.len()
                    && left.instructions.iter().zip(&right.instructions).all(
                        |(instruction_a, instruction_b)| {
                            if canonical {
                                normalized_instruction_content(instruction_a)
                                    == normalized_instruction_content(instruction_b)
                            } else {
                                raw_instruction_content(instruction_a)
                                    == raw_instruction_content(instruction_b)
                            }
                        },
                    )
            })
    };

    if same_sequence(false) {
        ContentMatch::Exact
    } else if same_sequence(true) {
        ContentMatch::Cosmetic
    } else {
        ContentMatch::Changed
    }
}

/// True for call-like mnemonics across common architectures.
pub(crate) fn is_call_mnemonic(m: &str) -> bool {
    let m = m.trim();
    m == "call"
        || m == "callq"
        || m == "calll"
        || m == "bl"
        || m == "blx"
        || matches!(m, "blr" | "blraa" | "blrab" | "blraaz" | "blrabz")
        || m == "jal"
        || m == "jalr"
}

/// True for return-like mnemonics across common architectures.
fn is_return_mnemonic(m: &str) -> bool {
    let m = m.trim();
    m.starts_with("ret") || m == "bx lr" || m == "jr ra"
}

fn is_informative_name(name: &str) -> bool {
    !is_generated_function_name(name)
}

impl DiffAlgorithms {
    pub(crate) fn function_features(func: &FunctionInfo) -> FunctionFeatures {
        let mut bb_mnemonic_counts = FxHashMap::default();
        let mut instruction_content_counts = FxHashMap::default();
        let mut constants = FxHashSet::default();
        let mut strings: FxHashSet<_> = func.strings.iter().cloned().collect();

        for block in &func.basic_blocks {
            *bb_mnemonic_counts
                .entry(block.mnemonic_hash.clone())
                .or_insert(0) += 1;
        }
        for instruction in &func.instructions {
            *instruction_content_counts
                .entry(normalized_instruction_content(instruction))
                .or_insert(0) += 1;
            for operand in &instruction.operands {
                let raw = operand.trim_start_matches('#');
                let value = if let Some(hex) = raw.strip_prefix("0x") {
                    u64::from_str_radix(hex, 16).ok()
                } else {
                    raw.parse::<i64>().ok().map(|value| value.unsigned_abs())
                };
                if let Some(value) = value {
                    if value < 0x10000 {
                        constants.insert(format!("{value:#x}"));
                    }
                }
                if operand.starts_with('"') && operand.ends_with('"') {
                    strings.insert(operand.clone());
                }
            }
        }

        let block_addresses: FxHashSet<u64> = func
            .basic_blocks
            .iter()
            .map(|block| block.address)
            .collect();
        let cfg_internal_edge_count = func
            .basic_blocks
            .iter()
            .flat_map(|block| block.edges.iter())
            .filter(|target| block_addresses.contains(target))
            .count();

        let instruction_sequence: Vec<_> = func
            .instructions
            .iter()
            .map(|instruction| stable_hash(&normalized_instruction_content(instruction)))
            .collect();
        let mut instruction_pairs = FxHashMap::default();
        for pair in instruction_sequence.windows(2) {
            *instruction_pairs.entry((pair[0], pair[1])).or_insert(0) += 1;
        }
        let (topology_nodes, topology_edges) = topology_histograms(func);
        FunctionFeatures {
            topology_nodes,
            topology_edges,
            instruction_pairs,
            instruction_sequence,
            bb_mnemonic_counts,
            instruction_content_counts,
            cfg_node_count: func.basic_blocks.len(),
            cfg_internal_edge_count,
            constants,
            strings,
        }
    }

    /// Calculate similarity between two functions using multiple metrics
    /// and return both the weighted score and detailed per-metric breakdown.
    pub fn compute_match_details(
        func_a: &FunctionInfo,
        func_b: &FunctionInfo,
    ) -> (f64, MatchDetails) {
        let features_a = Self::function_features(func_a);
        let features_b = Self::function_features(func_b);
        Self::compute_match_details_with_features(func_a, func_b, &features_a, &features_b)
    }

    pub(crate) fn compute_match_details_with_features(
        func_a: &FunctionInfo,
        func_b: &FunctionInfo,
        features_a: &FunctionFeatures,
        features_b: &FunctionFeatures,
    ) -> (f64, MatchDetails) {
        let cfg_similarity = sanitize_score(
            if !func_a.cfg_hash.is_empty() && func_a.cfg_hash == func_b.cfg_hash {
                1.0
            } else {
                features_a.structural_similarity(features_b)
            },
        );
        let multiset_similarity = |a: &FxHashMap<String, usize>,
                                   b: &FxHashMap<String, usize>,
                                   count_a: usize,
                                   count_b: usize| {
            if count_a == 0 && count_b == 0 {
                return 1.0;
            }
            if count_a == 0 || count_b == 0 {
                return 0.0;
            }
            let matched: usize = a
                .iter()
                .map(|(key, count)| count.min(b.get(key).unwrap_or(&0)))
                .sum();
            matched as f64 / count_a.max(count_b) as f64
        };
        let bb_similarity = sanitize_score(multiset_similarity(
            &features_a.bb_mnemonic_counts,
            &features_b.bb_mnemonic_counts,
            func_a.basic_blocks.len(),
            func_b.basic_blocks.len(),
        ));
        let instruction_multiset = sanitize_score(multiset_similarity(
            &features_a.instruction_content_counts,
            &features_b.instruction_content_counts,
            func_a.instructions.len(),
            func_b.instructions.len(),
        ));
        // Adjacent normalized instruction pairs retain operand order without
        // running a quadratic alignment for every retrieval candidate.
        let pair_count_a = features_a.instruction_sequence.len().saturating_sub(1);
        let pair_count_b = features_b.instruction_sequence.len().saturating_sub(1);
        let instruction_similarity = if pair_count_a.max(pair_count_b) > 0 {
            let overlap: usize = features_a
                .instruction_pairs
                .iter()
                .map(|(key, count)| count.min(features_b.instruction_pairs.get(key).unwrap_or(&0)))
                .sum();
            0.5 * instruction_multiset
                + 0.5 * overlap as f64 / pair_count_a.max(pair_count_b) as f64
        } else {
            instruction_multiset
        };
        let edge_similarity =
            histogram_overlap(&features_a.topology_edges, &features_b.topology_edges);
        let name_similarity =
            if is_informative_name(&func_a.name) && is_informative_name(&func_b.name) {
                sanitize_score(SimilarityAnalyzer::normalized_edit_distance(
                    &func_a.name,
                    &func_b.name,
                ))
            } else {
                0.0
            };
        let call_similarity =
            sanitize_score(SimilarityAnalyzer::function_call_similarity(func_a, func_b));

        // Missing features are not positive evidence and must not silently
        // award points. Re-normalize over only the metrics that carry signal.
        let mut weighted_sum = 0.0;
        let mut total_weight = 0.0;
        if !func_a.basic_blocks.is_empty() || !func_b.basic_blocks.is_empty() {
            weighted_sum += cfg_similarity * 0.30;
            total_weight += 0.30;
        }
        if !func_a.callees.is_empty() || !func_b.callees.is_empty() {
            weighted_sum += call_similarity * 0.20;
            total_weight += 0.20;
        }
        if !func_a.basic_blocks.is_empty() || !func_b.basic_blocks.is_empty() {
            weighted_sum += bb_similarity * 0.15;
            total_weight += 0.15;
        }
        if !func_a.instructions.is_empty() || !func_b.instructions.is_empty() {
            weighted_sum += instruction_similarity * 0.15;
            total_weight += 0.15;
        }
        // Names help generate and validate pairings, but they are not part of
        // body similarity. An exact body that was renamed must remain exact.
        let has_edges = |f: &FunctionInfo| f.basic_blocks.iter().any(|bb| !bb.edges.is_empty());
        if has_edges(func_a) || has_edges(func_b) {
            weighted_sum += edge_similarity * 0.10;
            total_weight += 0.10;
        }
        let weighted_similarity = if total_weight > 0.0 {
            sanitize_score(weighted_sum / total_weight)
        } else {
            0.0
        };

        let details = MatchDetails {
            neighbor_support: 0,
            neighbor_contradictions: 0,
            binding_anchor: false,
            cfg_similarity,
            bb_similarity,
            instruction_similarity,
            edge_similarity,
            name_similarity,
            call_similarity,
        };

        (weighted_similarity, details)
    }

    pub(crate) fn instruction_content_overlap(a: &FunctionFeatures, b: &FunctionFeatures) -> f64 {
        let overlap: usize = a
            .instruction_content_counts
            .iter()
            .map(|(key, count)| count.min(b.instruction_content_counts.get(key).unwrap_or(&0)))
            .sum();
        let count = a
            .instruction_sequence
            .len()
            .max(b.instruction_sequence.len());
        if count == 0 {
            0.0
        } else {
            overlap as f64 / count as f64
        }
    }

    pub(crate) fn retrieval_similarity(a: &FunctionFeatures, b: &FunctionFeatures) -> f64 {
        Self::instruction_content_overlap(a, b)
            + SimilarityAnalyzer::jaccard_similarity(&a.constants, &b.constants)
            + SimilarityAnalyzer::jaccard_similarity(&a.strings, &b.strings)
    }

    pub(crate) fn comprehensive_similarity_with_features(
        func_a: &FunctionInfo,
        func_b: &FunctionInfo,
        features_a: &FunctionFeatures,
        features_b: &FunctionFeatures,
    ) -> f64 {
        let cfg_similarity = features_a.structural_similarity(features_b);
        let mut weights = vec![(cfg_similarity, 0.3)];
        if !func_a.callees.is_empty() || !func_b.callees.is_empty() {
            weights.push((
                SimilarityAnalyzer::function_call_similarity(func_a, func_b),
                0.2,
            ));
        }
        if !features_a.constants.is_empty() || !features_b.constants.is_empty() {
            weights.push((
                SimilarityAnalyzer::jaccard_similarity(
                    &features_a.constants,
                    &features_b.constants,
                ),
                0.2,
            ));
        }
        if !features_a.strings.is_empty() || !features_b.strings.is_empty() {
            weights.push((
                SimilarityAnalyzer::jaccard_similarity(&features_a.strings, &features_b.strings),
                0.1,
            ));
        }
        if !func_a.instructions.is_empty() || !func_b.instructions.is_empty() {
            weights.push((
                SimilarityAnalyzer::instruction_sequence_similarity(
                    &func_a.instructions,
                    &func_b.instructions,
                ),
                0.2,
            ));
        }
        let (weighted, total) = weights
            .into_iter()
            .fold((0.0, 0.0), |(sum, total), (score, weight)| {
                (sum + score * weight, total + weight)
            });
        if total > 0.0 {
            weighted / total
        } else {
            0.0
        }
    }

    /// Calculate similarity between two functions (returns scalar only).
    pub fn calculate_function_similarity(func_a: &FunctionInfo, func_b: &FunctionInfo) -> f64 {
        let (similarity, _) = Self::compute_match_details(func_a, func_b);
        similarity
    }

    /// Score evidence that helps choose the correct counterpart without
    /// changing the reported body similarity. Meaningful names are useful for
    /// resolving ambiguous equal-body candidates; generated names carry no
    /// weight.
    pub(crate) fn pairing_score(
        func_a: &FunctionInfo,
        func_b: &FunctionInfo,
        similarity: f64,
        details: &MatchDetails,
    ) -> f64 {
        if is_informative_name(&func_a.name) && is_informative_name(&func_b.name) {
            sanitize_score(0.90 * similarity + 0.10 * details.name_similarity)
        } else {
            sanitize_score(similarity)
        }
    }

    /// Native confidence is determined by the algorithm that produced the
    /// match. Lower-confidence phases blend in the computed similarity so weak
    /// matches stay distinguishable. These values are not BinDiff confidence.
    pub fn confidence_for_match(match_type: &MatchType, similarity: f64) -> f64 {
        let s = sanitize_score(similarity);
        match match_type {
            MatchType::Exact => 1.0,
            MatchType::Name => 0.90 + 0.10 * s,
            MatchType::MdIndex => 0.80 + 0.10 * s,
            // The old 0.75 + 0.10*s calibration rounded almost every strong
            // SmallPrimes match to 85.0%, which made the score look hard-coded.
            // Keep the method below exact/name evidence while allowing the
            // candidate's measured similarity to affect the displayed result.
            MatchType::SmallPrimes => 0.45 + 0.45 * s,
            MatchType::Structural => 0.70 + 0.15 * s,
            MatchType::CallGraph => 0.50 + 0.35 * s,
            MatchType::Heuristic => 0.35 + 0.45 * s,
            MatchType::Manual => 1.0,
        }
    }

    /// Native Weisfeiler-Lehman-style labeled CFG hash. Blocks are labeled by structural category (out-degree
    /// class, contains-call, ends-in-return), then labels are refined for a
    /// few rounds with sorted neighbor labels. The final hash depends only on
    /// graph shape and block categories — not on addresses, block sizes,
    /// registers, or immediates.
    pub fn calculate_wl_cfg_hash(func: &FunctionInfo) -> String {
        let n = func.basic_blocks.len();
        if n == 0 {
            return "empty".to_string();
        }

        let addr_to_idx: FxHashMap<u64, usize> = func
            .basic_blocks
            .iter()
            .enumerate()
            .map(|(i, bb)| (bb.address, i))
            .collect();

        let mut succs: Vec<Vec<usize>> = vec![Vec::new(); n];
        let mut preds: Vec<Vec<usize>> = vec![Vec::new(); n];
        for (i, bb) in func.basic_blocks.iter().enumerate() {
            for target in &bb.edges {
                if let Some(&j) = addr_to_idx.get(target) {
                    succs[i].push(j);
                    preds[j].push(i);
                }
            }
        }

        // Initial labels: out-degree class (0/1/2/many), has-call, ends-in-return.
        let mut labels: Vec<u64> = func
            .basic_blocks
            .iter()
            .enumerate()
            .map(|(i, bb)| {
                let degree_class = succs[i].len().min(3) as u64;
                let has_call = bb
                    .instructions
                    .iter()
                    .any(|ins| is_call_mnemonic(&ins.mnemonic));
                let has_ret = bb
                    .instructions
                    .last()
                    .is_some_and(|ins| is_return_mnemonic(&ins.mnemonic));
                stable_hash(&(degree_class, has_call, has_ret))
            })
            .collect();

        // WL refinement: each round, relabel with own label + sorted neighbor labels.
        for _ in 0..3 {
            let mut next = Vec::with_capacity(n);
            for i in 0..n {
                let mut s: Vec<u64> = succs[i].iter().map(|&j| labels[j]).collect();
                let mut p: Vec<u64> = preds[i].iter().map(|&j| labels[j]).collect();
                s.sort_unstable();
                p.sort_unstable();
                next.push(stable_hash(&(labels[i], s, p)));
            }
            labels = next;
        }

        labels.sort_unstable();
        let mut hasher = Sha256::new();
        for label in &labels {
            hasher.update(label.to_le_bytes());
        }
        hex::encode(&hasher.finalize()[..8])
    }

    /// Native MD-inspired, position-independent topological fingerprint. This
    /// is neither BinDiff's graph MD attributes nor Diaphora's MD index. For
    /// each CFG edge, embed (entry BFS depth of source, in/out degrees of both
    /// endpoints) with distinct irrational multipliers and sum
    /// 1/sqrt(embedding). Robust to instruction-level changes; discriminates on
    /// structure.
    pub fn calculate_md_index(func: &FunctionInfo) -> String {
        let n = func.basic_blocks.len();
        if n == 0 {
            // Degenerate: address-salted so these never bucket together.
            return format!("md:empty:{:x}", func.address);
        }

        let addr_to_idx: FxHashMap<u64, usize> = func
            .basic_blocks
            .iter()
            .enumerate()
            .map(|(i, bb)| (bb.address, i))
            .collect();

        let mut succs: Vec<Vec<usize>> = vec![Vec::new(); n];
        let mut in_deg = vec![0usize; n];
        for (i, bb) in func.basic_blocks.iter().enumerate() {
            // Deterministic successor order regardless of extraction order.
            let mut targets: Vec<u64> = bb.edges.clone();
            targets.sort_unstable();
            for target in targets {
                if let Some(&j) = addr_to_idx.get(&target) {
                    succs[i].push(j);
                    in_deg[j] += 1;
                }
            }
        }

        // Shortest-path levels from the actual entry are independent of DFS
        // tie-breaking, block vector order, and successor addresses. Missing
        // entry/disconnected components retain a common unreachable level.
        let mut order = vec![n; n];
        let mut queue = std::collections::VecDeque::new();
        if let Some(&entry) = addr_to_idx.get(&func.address) {
            order[entry] = 0;
            queue.push_back(entry);
        } else {
            for (i, &degree) in in_deg.iter().enumerate() {
                if degree == 0 {
                    order[i] = 0;
                    queue.push_back(i);
                }
            }
        }
        while let Some(node) = queue.pop_front() {
            for &next in &succs[node] {
                if order[next] == n {
                    order[next] = order[node] + 1;
                    queue.push_back(next);
                }
            }
        }

        let sqrt2 = 2.0_f64.sqrt();
        let sqrt3 = 3.0_f64.sqrt();
        let sqrt5 = 5.0_f64.sqrt();
        let sqrt7 = 7.0_f64.sqrt();
        let sqrt11 = 11.0_f64.sqrt();

        let mut contributions = Vec::new();
        for (src, targets) in succs.iter().enumerate() {
            for &dst in targets {
                let embedding = sqrt2 * (order[src] + 1) as f64
                    + sqrt3 * succs[src].len() as f64
                    + sqrt5 * in_deg[src] as f64
                    + sqrt7 * succs[dst].len() as f64
                    + sqrt11 * in_deg[dst] as f64;
                if embedding > 0.0 {
                    contributions.push(1.0 / embedding.sqrt());
                }
            }
        }

        contributions.sort_by(f64::total_cmp);
        let md_index: f64 = contributions.iter().sum();
        if md_index == 0.0 {
            // No CFG edges (single-block function): the topological
            // fingerprint carries no information, so salt with the address
            // to keep unrelated leaf functions out of one giant bucket.
            return format!("md:leaf:{:x}", func.address);
        }

        // Fixed precision so float noise can't split buckets.
        format!("md:{:.8}", md_index)
    }

    /// Build a stable mnemonic -> prime table shared by both binaries:
    /// sorted unique mnemonics are assigned consecutive primes, so the same
    /// mnemonic always maps to the same prime on both sides of the diff.
    pub fn build_mnemonic_prime_table(
        functions_a: &[FunctionInfo],
        functions_b: &[FunctionInfo],
    ) -> FxHashMap<String, u64> {
        let mut mnemonics: FxHashSet<String> = FxHashSet::default();
        for func in functions_a.iter().chain(functions_b.iter()) {
            for instr in &func.instructions {
                mnemonics.insert(normalized_mnemonic(&instr.mnemonic));
            }
        }
        let mut sorted: Vec<String> = mnemonics.into_iter().collect();
        sorted.sort_unstable();

        let primes = Self::first_n_primes(sorted.len());
        sorted.into_iter().zip(primes).collect()
    }

    fn first_n_primes(n: usize) -> Vec<u64> {
        let mut primes: Vec<u64> = Vec::with_capacity(n);
        let mut candidate = 2u64;
        while primes.len() < n {
            if primes
                .iter()
                .take_while(|&&p| p * p <= candidate)
                .all(|&p| !candidate.is_multiple_of(p))
            {
                primes.push(candidate);
            }
            candidate += 1;
        }
        primes
    }

    /// Native small-primes mnemonic product. This pair-local prime table and
    /// modular product are not Diaphora's SPP. The value is computed modulo a
    /// Mersenne prime so the order-independence property survives arbitrarily
    /// large functions (the old u64 wrapping_mul silently destroyed it via
    /// overflow).
    pub fn calculate_small_primes_product(
        func: &FunctionInfo,
        prime_table: &FxHashMap<String, u64>,
    ) -> u64 {
        const MODULUS: u128 = (1u128 << 61) - 1;
        let mut product = 1u128;

        for instr in &func.instructions {
            let prime = prime_table
                .get(&normalized_mnemonic(&instr.mnemonic))
                .copied()
                .unwrap_or(1);
            product = (product * prime as u128) % MODULUS;
        }

        product as u64
    }

    /// Canonical instruction-content digest used by the strongest matching
    /// phase. CFG shape is keyed separately. Security-relevant literals remain
    /// exact; only operands explicitly typed as relocatable addresses and
    /// generated address symbols are canonicalized.
    pub fn calculate_instruction_content_hash(func: &FunctionInfo) -> String {
        let mut hasher = Sha256::new();
        for instr in &func.instructions {
            hasher.update(normalized_instruction_content(instr).as_bytes());
            hasher.update([0xfe]);
        }
        let result = hasher.finalize();
        hex::encode(&result[..8])
    }

    /// Structural equivalence test: WL-refined label multisets must agree.
    /// Far stronger than the old out-degree-histogram check (which accepted
    /// many non-isomorphic graphs), though still not an exact isomorphism test.
    pub fn is_isomorphic_subgraph(func_a: &FunctionInfo, func_b: &FunctionInfo) -> bool {
        if func_a.basic_blocks.len() != func_b.basic_blocks.len() {
            return false;
        }
        Self::calculate_wl_cfg_hash(func_a) == Self::calculate_wl_cfg_hash(func_b)
    }
}

#[cfg(test)]
mod tests {
    fn branch_graph() -> crate::FunctionInfo {
        crate::FunctionInfo {
            address: 100,
            basic_blocks: vec![
                crate::BasicBlockInfo {
                    address: 100,
                    edges: vec![110, 120],
                    ..Default::default()
                },
                crate::BasicBlockInfo {
                    address: 110,
                    edges: vec![130],
                    ..Default::default()
                },
                crate::BasicBlockInfo {
                    address: 120,
                    edges: vec![130, 140],
                    ..Default::default()
                },
                crate::BasicBlockInfo {
                    address: 130,
                    ..Default::default()
                },
                crate::BasicBlockInfo {
                    address: 140,
                    ..Default::default()
                },
            ],
            ..Default::default()
        }
    }

    #[test]
    fn md_and_topology_features_ignore_block_order_and_relocation() {
        let a = branch_graph();
        let expected = super::DiffAlgorithms::calculate_md_index(&a);
        let mut b = a.clone();
        b.basic_blocks.swap(0, 2);
        assert_eq!(expected, super::DiffAlgorithms::calculate_md_index(&b));
        for block in &mut b.basic_blocks {
            if block.address == 110 {
                block.address = 125;
            }
            for edge in &mut block.edges {
                if *edge == 110 {
                    *edge = 125;
                }
            }
            block.edges.reverse();
        }
        assert_eq!(expected, super::DiffAlgorithms::calculate_md_index(&b));
        let (_, details) = super::DiffAlgorithms::compute_match_details(&a, &b);
        assert_eq!(details.cfg_similarity, 1.0);
        assert_eq!(details.edge_similarity, 1.0);
    }

    #[test]
    fn equal_graph_counts_do_not_imply_perfect_topology_similarity() {
        let a = branch_graph();
        let mut b = a.clone();
        b.basic_blocks[0].edges = vec![120, 130];
        b.basic_blocks[2].edges = vec![110, 140];
        let (_, details) = super::DiffAlgorithms::compute_match_details(&a, &b);
        assert!(details.cfg_similarity < 1.0);
        assert!(details.edge_similarity < 1.0);
    }

    #[test]
    fn absent_body_evidence_does_not_award_similarity() {
        let (score, _) = super::DiffAlgorithms::compute_match_details(
            &crate::FunctionInfo::default(),
            &crate::FunctionInfo::default(),
        );
        assert_eq!(score, 0.0);
    }

    #[test]
    fn reordered_operand_writes_reduce_similarity() {
        let instructions: Vec<_> = ["1", "2"]
            .iter()
            .map(|value| crate::InstructionInfo {
                mnemonic: "mov".into(),
                operands: vec!["eax".into(), (*value).into()],
                ..Default::default()
            })
            .collect();
        let a = crate::FunctionInfo {
            instructions: instructions.clone(),
            basic_blocks: vec![crate::BasicBlockInfo {
                instructions,
                ..Default::default()
            }],
            ..Default::default()
        };
        let mut b = a.clone();
        b.instructions.reverse();
        b.basic_blocks[0].instructions.reverse();
        assert!(super::DiffAlgorithms::calculate_function_similarity(&a, &b) < 1.0);
        assert_eq!(
            super::classify_content_match(&a, &b),
            crate::types::ContentMatch::Changed
        );
    }

    use super::*;
    use crate::{BasicBlockInfo, InstructionInfo};

    fn one_instruction_function(name: &str, address: u64) -> FunctionInfo {
        let instruction = InstructionInfo {
            address,
            mnemonic: "ret".into(),
            length: 1,
            ..Default::default()
        };
        FunctionInfo {
            name: name.into(),
            address,
            basic_blocks: vec![BasicBlockInfo {
                address,
                size: 1,
                instructions: vec![instruction.clone()],
                instruction_count: 1,
                ..Default::default()
            }],
            instructions: vec![instruction],
            instruction_count: 1,
            ..Default::default()
        }
    }

    #[test]
    fn generated_name_does_not_penalize_symbolized_body_match() {
        let stripped = one_instruction_function("sub_1000", 0x1000);
        let symbolized = one_instruction_function("parse_header", 0x2000);
        let (similarity, details) = DiffAlgorithms::compute_match_details(&stripped, &symbolized);
        assert!((similarity - 1.0).abs() < 1e-9);
        assert!(details.name_similarity < 1.0);
    }

    #[test]
    fn meaningful_rename_does_not_reduce_body_similarity() {
        let a = one_instruction_function("parse_header", 0x1000);
        let b = one_instruction_function("write_footer", 0x2000);
        let (similarity, details) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!((similarity - 1.0).abs() < 1e-9);
        assert!(details.name_similarity < 1.0);
    }

    #[test]
    fn operand_change_reduces_body_similarity() {
        let mut a = one_instruction_function("sub_1000", 0x1000);
        let mut b = one_instruction_function("sub_2000", 0x2000);
        a.instructions[0].mnemonic = "cmp".into();
        b.instructions[0].mnemonic = "cmp".into();
        a.instructions[0].operands = vec!["eax".into(), "1".into()];
        b.instructions[0].operands = vec!["eax".into(), "2".into()];
        a.basic_blocks[0].instructions = a.instructions.clone();
        b.basic_blocks[0].instructions = b.instructions.clone();
        let (similarity, details) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!(similarity < 1.0);
        assert_eq!(details.instruction_similarity, 0.0);
    }

    #[test]
    fn relocated_large_address_does_not_reduce_body_similarity() {
        let mut a = one_instruction_function("sub_1000", 0x1000);
        let mut b = one_instruction_function("sub_2000", 0x2000);
        a.instructions[0].mnemonic = "call".into();
        b.instructions[0].mnemonic = "call".into();
        a.instructions[0].operands = vec!["0x140001000".into()];
        b.instructions[0].operands = vec!["0x180002000".into()];
        a.instructions[0].operand_types = vec![5];
        b.instructions[0].operand_types = vec![5];
        a.basic_blocks[0].instructions = a.instructions.clone();
        b.basic_blocks[0].instructions = b.instructions.clone();
        let (similarity, _) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!((similarity - 1.0).abs() < 1e-9);
    }

    #[test]
    fn relocated_generated_data_symbol_does_not_reduce_body_similarity() {
        let mut a = one_instruction_function("read_flag", 0x1000);
        let mut b = one_instruction_function("read_flag", 0x2000);
        a.instructions[0].mnemonic = "mov".into();
        b.instructions[0].mnemonic = "mov".into();
        a.instructions[0].operands = vec!["eax".into(), "data_140001000".into()];
        b.instructions[0].operands = vec!["eax".into(), "data_180002000".into()];
        a.instructions[0].operand_types = vec![3, 65];
        b.instructions[0].operand_types = vec![3, 65];
        a.basic_blocks[0].instructions = a.instructions.clone();
        b.basic_blocks[0].instructions = b.instructions.clone();
        let (similarity, details) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!((similarity - 1.0).abs() < 1e-9);
        assert_eq!(details.instruction_similarity, 1.0);
    }

    #[test]
    fn relocated_generated_code_symbol_does_not_reduce_body_similarity() {
        let mut a = one_instruction_function("dispatch", 0x1000);
        let mut b = one_instruction_function("dispatch", 0x2000);
        a.instructions[0].mnemonic = "call".into();
        b.instructions[0].mnemonic = "call".into();
        a.instructions[0].operands = vec!["sub_140001000".into()];
        b.instructions[0].operands = vec!["sub_180002000".into()];
        a.instructions[0].operand_types = vec![64];
        b.instructions[0].operand_types = vec![64];
        a.basic_blocks[0].instructions = a.instructions.clone();
        b.basic_blocks[0].instructions = b.instructions.clone();
        let (similarity, details) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!((similarity - 1.0).abs() < 1e-9);
        assert_eq!(details.instruction_similarity, 1.0);
    }

    #[test]
    fn meaningful_symbol_change_remains_patch_evidence() {
        let mut a = one_instruction_function("dispatch", 0x1000);
        let mut b = one_instruction_function("dispatch", 0x2000);
        a.instructions[0].mnemonic = "call".into();
        b.instructions[0].mnemonic = "call".into();
        a.instructions[0].operands = vec!["memcpy".into()];
        b.instructions[0].operands = vec!["memmove".into()];
        a.instructions[0].operand_types = vec![64];
        b.instructions[0].operand_types = vec![64];
        a.basic_blocks[0].instructions = a.instructions.clone();
        b.basic_blocks[0].instructions = b.instructions.clone();
        let (similarity, details) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!(similarity < 1.0);
        assert_eq!(details.instruction_similarity, 0.0);
    }

    #[test]
    fn meaningful_data_symbol_change_remains_patch_evidence() {
        let mut a = one_instruction_function("read_table", 0x1000);
        let mut b = one_instruction_function("read_table", 0x2000);
        a.instructions[0].mnemonic = "mov".into();
        b.instructions[0].mnemonic = "mov".into();
        a.instructions[0].operands = vec!["data_buffer".into()];
        b.instructions[0].operands = vec!["data_limit".into()];
        a.instructions[0].operand_types = vec![65];
        b.instructions[0].operand_types = vec![65];
        a.basic_blocks[0].instructions = a.instructions.clone();
        b.basic_blocks[0].instructions = b.instructions.clone();
        let (similarity, details) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!(similarity < 1.0);
        assert_eq!(details.instruction_similarity, 0.0);
    }

    #[test]
    fn generated_looking_user_symbol_without_known_prefix_remains_patch_evidence() {
        let mut a = one_instruction_function("read_table", 0x1000);
        let mut b = one_instruction_function("read_table", 0x2000);
        a.instructions[0].mnemonic = "mov".into();
        b.instructions[0].mnemonic = "mov".into();
        a.instructions[0].operands = vec!["database_deadbeef".into()];
        b.instructions[0].operands = vec!["database_cafebabe".into()];
        a.instructions[0].operand_types = vec![65];
        b.instructions[0].operand_types = vec![65];
        a.basic_blocks[0].instructions = a.instructions.clone();
        b.basic_blocks[0].instructions = b.instructions.clone();
        let (similarity, details) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!(similarity < 1.0);
        assert_eq!(details.instruction_similarity, 0.0);
    }

    #[test]
    fn changed_large_integer_remains_patch_evidence() {
        let mut a = one_instruction_function("sub_1000", 0x1000);
        let mut b = one_instruction_function("sub_2000", 0x2000);
        a.instructions[0].mnemonic = "cmp".into();
        b.instructions[0].mnemonic = "cmp".into();
        a.instructions[0].operands = vec!["eax".into(), "0x10000".into()];
        b.instructions[0].operands = vec!["eax".into(), "0x20000".into()];
        a.instructions[0].operand_types = vec![3, 4];
        b.instructions[0].operand_types = vec![3, 4];
        a.basic_blocks[0].instructions = a.instructions.clone();
        b.basic_blocks[0].instructions = b.instructions.clone();
        let (similarity, details) = DiffAlgorithms::compute_match_details(&a, &b);
        assert!(similarity < 1.0);
        assert_eq!(details.instruction_similarity, 0.0);
    }

    #[test]
    fn content_status_separates_exact_relocations_and_real_changes() {
        let exact_a = one_instruction_function("read_table", 0x1000);
        let exact_b = one_instruction_function("read_table", 0x2000);
        assert_eq!(
            classify_content_match(&exact_a, &exact_b),
            ContentMatch::Exact
        );

        let mut relocated_a = exact_a.clone();
        let mut relocated_b = exact_b.clone();
        relocated_a.instructions[0].mnemonic = "mov".into();
        relocated_b.instructions[0].mnemonic = "mov".into();
        relocated_a.instructions[0].operands = vec!["data_140001000".into()];
        relocated_b.instructions[0].operands = vec!["data_180002000".into()];
        relocated_a.instructions[0].operand_types = vec![65];
        relocated_b.instructions[0].operand_types = vec![65];
        relocated_a.basic_blocks[0].instructions = relocated_a.instructions.clone();
        relocated_b.basic_blocks[0].instructions = relocated_b.instructions.clone();
        assert_eq!(
            classify_content_match(&relocated_a, &relocated_b),
            ContentMatch::Cosmetic
        );

        relocated_b.instructions[0].operands = vec!["data_limit".into()];
        relocated_b.basic_blocks[0].instructions = relocated_b.instructions.clone();
        assert_eq!(
            classify_content_match(&relocated_a, &relocated_b),
            ContentMatch::Changed
        );
    }

    #[test]
    fn incoming_callers_do_not_change_a_callee_body_status() {
        let mut a = one_instruction_function("memset", 0x1000);
        let mut b = one_instruction_function("memset", 0x2000);
        a.callers = vec![0x3000];
        b.callers = vec![0x4000, 0x5000];
        assert_eq!(classify_content_match(&a, &b), ContentMatch::Exact);
    }

    #[test]
    fn small_primes_key_is_order_and_case_independent_but_counts_multiplicity() {
        let make = |mnemonics: &[&str]| FunctionInfo {
            instructions: mnemonics
                .iter()
                .map(|mnemonic| InstructionInfo {
                    mnemonic: (*mnemonic).into(),
                    ..Default::default()
                })
                .collect(),
            ..Default::default()
        };
        let a = make(&["mov", "ADD", "ret"]);
        let b = make(&["ret", "add", "MOV"]);
        let c = make(&["ret", "add", "mov", "mov"]);
        let table = DiffAlgorithms::build_mnemonic_prime_table(
            std::slice::from_ref(&a),
            &[b.clone(), c.clone()],
        );
        assert_eq!(
            DiffAlgorithms::calculate_small_primes_product(&a, &table),
            DiffAlgorithms::calculate_small_primes_product(&b, &table)
        );
        assert_ne!(
            DiffAlgorithms::calculate_small_primes_product(&a, &table),
            DiffAlgorithms::calculate_small_primes_product(&c, &table)
        );
    }
}

#[cfg(test)]
mod authenticated_call_tests {
    use super::is_call_mnemonic;
    #[test]
    fn arm64_authenticated_calls_are_calls() {
        for mnemonic in ["blraa", "blrab", "blraaz", "blrabz"] {
            assert!(is_call_mnemonic(mnemonic));
        }
        for mnemonic in ["braa", "brab", "retaa", "retab"] {
            assert!(!is_call_mnemonic(mnemonic));
        }
    }
}
