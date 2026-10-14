use anyhow::{bail, Context, Result};
use log::info;
use rayon::prelude::*;
use std::collections::HashSet;
use std::time::Instant;

pub mod algorithms;
pub mod block;
pub mod database;
pub mod ffi;
pub mod il;
pub mod matching;
pub mod security;
pub mod similarity;
mod suggestions;
pub mod types;
pub mod ui;

pub use algorithms::*;
pub use similarity::*;
pub use types::*;

/// Immutable binary snapshot. Safe to share between concurrent comparisons.
/// It is process-local and must be rebuilt when the extraction revision changes.
pub struct PreparedBinary {
    functions: Vec<FunctionInfo>,
    matching: matching::PreparedMatching,
    estimated_bytes: usize,
}

impl PreparedBinary {
    pub fn from_json(json: &str, cancelled: &(dyn Fn() -> bool + Sync)) -> Result<Self> {
        if cancelled() {
            bail!("binary preparation cancelled");
        }
        let mut functions: Vec<FunctionInfo> =
            serde_json::from_str(json).context("Failed to parse functions")?;
        BinaryDiffEngine::preprocess_functions_cancellable(&mut functions, cancelled)?;
        let matching = matching::PreparedMatching::new(&functions, cancelled)?;
        let estimated_bytes = std::mem::size_of::<Self>()
            + functions.capacity() * std::mem::size_of::<FunctionInfo>()
            + functions
                .iter()
                .map(FunctionInfo::estimated_heap_bytes)
                .sum::<usize>()
            + matching.estimated_heap_bytes();
        Ok(Self {
            functions,
            matching,
            estimated_bytes,
        })
    }

    /// Accounted capacity estimate, not a measurement of allocator RSS.
    pub fn estimated_bytes(&self) -> usize {
        self.estimated_bytes
    }
}

pub struct BinaryDiffEngine {
    pub similarity_threshold: f64,
    pub confidence_threshold: f64,
}

impl Default for BinaryDiffEngine {
    fn default() -> Self {
        Self::new()
    }
}

impl BinaryDiffEngine {
    pub fn new() -> Self {
        Self {
            similarity_threshold: 0.6,
            confidence_threshold: 0.5,
        }
    }

    pub fn with_thresholds(similarity: f64, confidence: f64) -> Self {
        Self {
            similarity_threshold: similarity,
            confidence_threshold: confidence,
        }
    }

    /// Normalize extracted functions: structure-only hashes are computed here
    /// (not in the extractor) so they are deterministic and consistent across
    /// frontends. cfg_hash = WL labeled-graph hash, call_graph_hash = local
    /// call-graph degree signature.
    pub fn preprocess_functions(functions: &mut [FunctionInfo]) {
        Self::preprocess_functions_cancellable(functions, &|| false).unwrap();
    }

    fn preprocess_functions_cancellable(
        functions: &mut [FunctionInfo],
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<()> {
        use sha2::{Digest, Sha256};
        functions.par_iter_mut().try_for_each(|func| {
            if cancelled() {
                bail!("binary preparation cancelled");
            }
            // The native extractor sends each instruction only in its owning
            // basic block to keep the FFI payload compact. Other frontends may
            // still provide the historical flat list, so reconstruct it only
            // when absent.
            if func.instructions.is_empty() {
                func.instructions = func
                    .basic_blocks
                    .iter()
                    .flat_map(|block| block.instructions.iter().cloned())
                    .collect();
            }
            func.cfg_hash = DiffAlgorithms::calculate_wl_cfg_hash(func);
            func.call_graph_hash = format!("cg:{}:{}", func.callees.len(), func.callers.len());
            func.instruction_count = func.instructions.len();
            func.call_count = func.callees.len();
            for bb in func.basic_blocks.iter_mut() {
                bb.instruction_count = bb.instructions.len();
                if bb.mnemonic_hash.is_empty() {
                    let mut hasher = Sha256::new();
                    for ins in &bb.instructions {
                        hasher.update(ins.mnemonic.as_bytes());
                        hasher.update(b" ");
                    }
                    bb.mnemonic_hash = hex::encode(&hasher.finalize()[..8]);
                }
            }
            Ok(())
        })
    }

    /// Diff two binaries from JSON-encoded `Vec<FunctionInfo>` (the JSON FFI
    /// path used by the native plugin).
    pub fn perform_diff_json(&self, json_a: &str, json_b: &str) -> Result<DiffResult> {
        self.perform_diff_json_cancellable(json_a, json_b, &|| false)
    }

    /// Cancellable variant used by the native UI. The callback is intentionally
    /// lightweight because the matcher polls it from Rayon worker threads.
    pub fn perform_diff_json_cancellable(
        &self,
        json_a: &str,
        json_b: &str,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<DiffResult> {
        let a = PreparedBinary::from_json(json_a, cancelled)?;
        let b = PreparedBinary::from_json(json_b, cancelled)?;
        self.perform_diff_prepared(&a, &b, cancelled)
    }

    pub fn perform_diff_prepared(
        &self,
        a: &PreparedBinary,
        b: &PreparedBinary,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<DiffResult> {
        let functions_a = &a.functions;
        let functions_b = &b.functions;
        let start_time = Instant::now();
        let total_functions_a = functions_a.len();
        let total_functions_b = functions_b.len();

        info!(
            "Diffing {} functions against {}",
            functions_a.len(),
            functions_b.len()
        );

        let engine = matching::MatchingEngine::with_thresholds(
            self.confidence_threshold,
            self.similarity_threshold,
        );
        let mut matches = engine.match_prepared(
            functions_a,
            &a.matching,
            functions_b,
            &b.matching,
            cancelled,
        )?;

        let security = security::assess_diff(functions_a, functions_b, &mut matches);

        let matched_a: HashSet<u64> = matches.iter().map(|m| m.function_a.address).collect();
        let matched_b: HashSet<u64> = matches.iter().map(|m| m.function_b.address).collect();

        let mut unmatched_a: Vec<FunctionInfo> = functions_a
            .iter()
            .filter(|f| !matched_a.contains(&f.address))
            .map(|f| {
                let mut slim = f.slimmed_clone();
                slim.strings = f.strings.clone();
                slim
            })
            .collect();

        let mut unmatched_b: Vec<FunctionInfo> = functions_b
            .iter()
            .filter(|f| !matched_b.contains(&f.address))
            .map(|f| {
                let mut slim = f.slimmed_clone();
                slim.strings = f.strings.clone();
                slim
            })
            .collect();

        for function in &mut unmatched_a {
            if let Some(assessment) = security.removed.get(&function.address) {
                function.security = assessment.clone();
            }
        }
        for function in &mut unmatched_b {
            if let Some(assessment) = security.added.get(&function.address) {
                function.security = assessment.clone();
            }
        }

        let review_suggestions = suggestions::find_review_suggestions(
            functions_a,
            functions_b,
            &matches,
            &unmatched_a,
            &unmatched_b,
            cancelled,
        )?;

        let matched_similarity_score = if !matches.is_empty() {
            matches.iter().map(|m| m.similarity).sum::<f64>() / matches.len() as f64
        } else if total_functions_a == 0 && total_functions_b == 0 {
            1.0
        } else {
            0.0
        };
        let total_functions = total_functions_a.max(total_functions_b);
        let match_coverage = if total_functions == 0 {
            1.0
        } else {
            matches.len() as f64 / total_functions as f64
        };
        // Coverage-adjusted binary similarity: unmatched functions contribute
        // zero instead of disappearing from the aggregate.
        let similarity_score = matched_similarity_score * match_coverage;

        let analysis_time = start_time.elapsed().as_secs_f64();

        info!(
            "Diff analysis completed in {:.2}s: {} matches, similarity: {:.3}",
            analysis_time,
            matches.len(),
            similarity_score
        );

        Ok(DiffResult {
            matched_functions: matches,
            unmatched_functions_a: unmatched_a,
            unmatched_functions_b: unmatched_b,
            review_suggestions,
            similarity_score,
            matched_similarity_score,
            match_coverage,
            analysis_time,
            binary_a_name: "binary_a".to_string(),
            binary_b_name: "binary_b".to_string(),
        })
    }

    pub fn save_results(&self, diff_result: &DiffResult, output_path: &str) -> Result<()> {
        let json_data = serde_json::to_string_pretty(diff_result)
            .context("Failed to serialize diff results")?;

        std::fs::write(output_path, json_data).context("Failed to write results file")?;

        info!("Results saved to {}", output_path);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn function(name: &str, address: u64, operand: &str) -> FunctionInfo {
        let instructions = vec![
            InstructionInfo {
                address,
                mnemonic: "mov".into(),
                operands: vec!["eax".into(), operand.into()],
                length: 5,
                ..Default::default()
            },
            InstructionInfo {
                address: address + 5,
                mnemonic: "ret".into(),
                length: 1,
                ..Default::default()
            },
        ];
        FunctionInfo {
            name: name.into(),
            address,
            size: 6,
            basic_blocks: vec![BasicBlockInfo {
                address,
                size: 6,
                instructions: instructions.clone(),
                ..Default::default()
            }],
            instructions,
            cyclomatic_complexity: 1,
            ..Default::default()
        }
    }

    fn diff(a: Vec<FunctionInfo>, b: Vec<FunctionInfo>) -> DiffResult {
        BinaryDiffEngine::new()
            .perform_diff_json(
                &serde_json::to_string(&a).unwrap(),
                &serde_json::to_string(&b).unwrap(),
            )
            .unwrap()
    }

    #[test]
    fn changed_operand_is_not_exact() {
        let result = diff(
            vec![function("sub_1000", 0x1000, "1")],
            vec![function("sub_2000", 0x2000, "2")],
        );
        assert!(result.matched_functions.is_empty());
    }

    #[test]
    fn aggregate_similarity_penalizes_unmatched_functions() {
        let result = diff(
            vec![
                function("sub_1000", 0x1000, "1"),
                function("only_a", 0x1100, "2"),
            ],
            vec![function("sub_2000", 0x2000, "1")],
        );
        assert_eq!(result.matched_functions.len(), 1);
        assert!((result.matched_similarity_score - 1.0).abs() < 1e-9);
        assert!((result.match_coverage - 0.5).abs() < 1e-9);
        assert!((result.similarity_score - 0.5).abs() < 1e-9);
    }

    #[test]
    fn block_only_wire_shape_reconstructs_flat_instructions() {
        let mut a = function("sub_1000", 0x1000, "1");
        let mut b = function("sub_2000", 0x2000, "1");
        a.instructions.clear();
        b.instructions.clear();
        let result = diff(vec![a], vec![b]);
        assert_eq!(result.matched_functions.len(), 1);
        assert_eq!(result.matched_functions[0].match_type, MatchType::Exact);
        assert_eq!(result.matched_functions[0].function_a.instruction_count, 2);
        assert_eq!(result.matched_functions[0].function_b.instruction_count, 2);
    }

    #[test]
    fn prepared_snapshots_are_reusable_across_pairs_and_threads() {
        let mut extra = function("extra", 0x3000, "3");
        extra.strings = vec!["keep unmatched string evidence".into()];
        let json_a = serde_json::to_string(&vec![extra, function("leaf", 0x1000, "1")]).unwrap();
        let json_b = serde_json::to_string(&vec![function("leaf", 0x2000, "2")]).unwrap();
        let a = PreparedBinary::from_json(&json_a, &|| false).unwrap();
        let b = PreparedBinary::from_json(&json_b, &|| false).unwrap();
        let empty = PreparedBinary::from_json("[]", &|| false).unwrap();
        let normalize = |mut result: DiffResult| {
            result.analysis_time = 0.0;
            serde_json::to_value(result).unwrap()
        };
        let engine = BinaryDiffEngine::new();
        let expected = normalize(engine.perform_diff_json(&json_a, &json_b).unwrap());
        std::thread::scope(|scope| {
            for _ in 0..4 {
                let expected = &expected;
                let a = &a;
                let b = &b;
                let empty = &empty;
                scope.spawn(move || {
                    let engine = BinaryDiffEngine::new();
                    let unmatched = engine.perform_diff_prepared(a, empty, &|| false).unwrap();
                    assert_eq!(unmatched.unmatched_functions_a[0].address, 0x3000);
                    assert_eq!(
                        unmatched.unmatched_functions_a[0].strings,
                        vec!["keep unmatched string evidence"]
                    );
                    assert_eq!(
                        &normalize(engine.perform_diff_prepared(a, b, &|| false).unwrap()),
                        expected
                    );
                });
            }
        });
    }
}
