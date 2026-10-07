use crate::algorithms::DiffAlgorithms;
use crate::types::{FunctionInfo, FunctionMatch, MatchSuggestion};
use anyhow::{bail, Result};
use rustc_hash::FxHashMap;

/// Offer a review hint when a still-unmatched pair has independent caller and
/// callee witnesses. Hints never enter the match map or the security assessment.
pub(crate) fn find_review_suggestions(
    functions_a: &[FunctionInfo],
    functions_b: &[FunctionInfo],
    matches: &[FunctionMatch],
    unmatched_a: &[FunctionInfo],
    unmatched_b: &[FunctionInfo],
    cancelled: &(dyn Fn() -> bool + Sync),
) -> Result<Vec<MatchSuggestion>> {
    const MAX_CALLER_BUCKET: usize = 64;
    const MIN_CONTENT_OVERLAP: f64 = 0.10;
    if unmatched_a.is_empty() || unmatched_b.is_empty() {
        return Ok(Vec::new());
    }

    let indices_a: FxHashMap<u64, usize> = functions_a
        .iter()
        .enumerate()
        .map(|(index, function)| (function.address, index))
        .collect();
    let indices_b: FxHashMap<u64, usize> = functions_b
        .iter()
        .enumerate()
        .map(|(index, function)| (function.address, index))
        .collect();
    let mut accepted_a = FxHashMap::default();
    let mut accepted_b = FxHashMap::default();
    let mut witnesses_a = FxHashMap::default();
    let mut witnesses_b = FxHashMap::default();
    for matched in matches {
        let a = matched.function_a.address;
        let b = matched.function_b.address;
        accepted_a.insert(a, b);
        accepted_b.insert(b, a);
        let reliable = matched.details.binding_anchor
            || (matched.similarity >= 0.90
                && matched.confidence >= 0.85
                && matched.ambiguity_margin >= 0.10
                && matched.details.neighbor_contradictions == 0);
        if reliable {
            witnesses_a.insert(a, (b, matched.details.binding_anchor));
            witnesses_b.insert(b, (a, matched.details.binding_anchor));
        }
    }
    if witnesses_a.is_empty() {
        return Ok(Vec::new());
    }

    let mut by_caller: FxHashMap<u64, Vec<usize>> = FxHashMap::default();
    for (index, function) in unmatched_b.iter().enumerate() {
        for &caller in &function.callers {
            if witnesses_b.contains_key(&caller) {
                by_caller.entry(caller).or_default().push(index);
            }
        }
    }

    let coherent = |a: &FunctionInfo, b: &FunctionInfo| {
        for (left, right) in [(&a.callers, &b.callers), (&a.callees, &b.callees)] {
            if left.iter().any(|neighbor| {
                accepted_a
                    .get(neighbor)
                    .is_some_and(|mapped| !right.contains(mapped))
            }) || right.iter().any(|neighbor| {
                accepted_b
                    .get(neighbor)
                    .is_some_and(|mapped| !left.contains(mapped))
            }) {
                return false;
            }
        }
        true
    };

    let mut candidates: FxHashMap<(usize, usize), f64> = FxHashMap::default();
    for (a_index, summary_a) in unmatched_a.iter().enumerate() {
        if cancelled() {
            bail!("function suggestion cancelled");
        }
        let Some(&source_index) = indices_a.get(&summary_a.address) else {
            continue;
        };
        let source = &functions_a[source_index];
        if source.instructions.len() < 5 {
            continue;
        }
        let mut features_a = None;
        for &caller_a in &source.callers {
            let Some(&(caller_b, caller_binding)) = witnesses_a.get(&caller_a) else {
                continue;
            };
            let Some(bucket) = by_caller.get(&caller_b) else {
                continue;
            };
            if bucket.len() > MAX_CALLER_BUCKET {
                continue;
            }
            let Some(&parent_a) = indices_a.get(&caller_a) else {
                continue;
            };
            if !functions_a[parent_a].callees.contains(&source.address) {
                continue;
            }
            for &b_index in bucket {
                if cancelled() {
                    bail!("function suggestion cancelled");
                }
                let Some(&target_index) = indices_b.get(&unmatched_b[b_index].address) else {
                    continue;
                };
                let target = &functions_b[target_index];
                if target.instructions.len() < 5
                    || !functions_b[indices_b[&caller_b]]
                        .callees
                        .contains(&target.address)
                    || !coherent(source, target)
                {
                    continue;
                }
                let callee_witness = source.callees.iter().any(|callee_a| {
                    let Some(&(callee_b, callee_binding)) = witnesses_a.get(callee_a) else {
                        return false;
                    };
                    if !caller_binding && !callee_binding {
                        return false;
                    }
                    target.callees.contains(&callee_b)
                        && indices_a.get(callee_a).is_some_and(|&index| {
                            functions_a[index].callers.contains(&source.address)
                        })
                        && indices_b.get(&callee_b).is_some_and(|&index| {
                            functions_b[index].callers.contains(&target.address)
                        })
                });
                if !callee_witness {
                    continue;
                }
                let source_features =
                    features_a.get_or_insert_with(|| DiffAlgorithms::function_features(source));
                let features_b = DiffAlgorithms::function_features(target);
                if DiffAlgorithms::instruction_content_overlap(source_features, &features_b)
                    < MIN_CONTENT_OVERLAP
                {
                    continue;
                }
                let (similarity, _) = DiffAlgorithms::compute_match_details_with_features(
                    source,
                    target,
                    source_features,
                    &features_b,
                );
                candidates.insert((a_index, b_index), similarity);
            }
        }
    }

    let mut count_a = vec![0usize; unmatched_a.len()];
    let mut count_b = vec![0usize; unmatched_b.len()];
    for &(a, b) in candidates.keys() {
        count_a[a] += 1;
        count_b[b] += 1;
    }
    let mut suggestions = Vec::new();
    for ((a, b), similarity) in candidates {
        if count_a[a] != 1 || count_b[b] != 1 {
            continue;
        }
        suggestions.push(MatchSuggestion {
            a_address: unmatched_a[a].address,
            b_address: unmatched_b[b].address,
            body_similarity: similarity,
        });
    }
    suggestions.sort_unstable_by_key(|suggestion| (suggestion.a_address, suggestion.b_address));
    Ok(suggestions)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::BinaryDiffEngine;

    const REFERENCE: &str = include_str!("../../tests/fixtures/memcpy_reference_extract.json");
    const TARGET: &str =
        include_str!("../../tests/fixtures/memcpy_vulnerable_stripped_extract.json");

    fn inputs() -> (Vec<FunctionInfo>, Vec<FunctionInfo>) {
        let mut a: Vec<FunctionInfo> = serde_json::from_str(REFERENCE).unwrap();
        let mut b: Vec<FunctionInfo> = serde_json::from_str(TARGET).unwrap();
        BinaryDiffEngine::preprocess_functions(&mut a);
        BinaryDiffEngine::preprocess_functions(&mut b);
        (a, b)
    }

    #[test]
    fn changed_memcpy_wrapper_is_suggested_without_becoming_an_accepted_match() {
        let engine = BinaryDiffEngine::new();
        let forward = engine.perform_diff_json(REFERENCE, TARGET).unwrap();
        let reverse = engine.perform_diff_json(TARGET, REFERENCE).unwrap();
        assert_eq!(forward.matched_functions.len(), 4);
        assert!(forward
            .matched_functions
            .iter()
            .all(|matched| matched.function_a.address != 0x1000005ac));
        assert_eq!(forward.review_suggestions.len(), 1);
        let suggestion = &forward.review_suggestions[0];
        assert_eq!(
            (suggestion.a_address, suggestion.b_address),
            (0x1000005ac, 0x1000004c4)
        );
        assert!(suggestion.body_similarity < 0.35);
        assert_eq!(
            (
                reverse.review_suggestions[0].a_address,
                reverse.review_suggestions[0].b_address
            ),
            (suggestion.b_address, suggestion.a_address)
        );
    }

    #[test]
    fn competing_or_unrelated_graph_candidates_are_not_suggested() {
        let result = BinaryDiffEngine::new()
            .perform_diff_json(REFERENCE, TARGET)
            .unwrap();
        let (a, mut b) = inputs();
        let mut copy = b
            .iter()
            .find(|function| function.address == 0x1000004c4)
            .unwrap()
            .clone();
        copy.address = 0x100010000;
        copy.name = "sub_100010000".into();
        b.iter_mut()
            .find(|function| function.name == "_main")
            .unwrap()
            .callees
            .push(copy.address);
        b.iter_mut()
            .find(|function| function.name == "___memcpy_chk")
            .unwrap()
            .callers
            .push(copy.address);
        let unmatched_a = result.unmatched_functions_a.clone();
        let mut unmatched_b = result.unmatched_functions_b.clone();
        unmatched_b.push(copy.slimmed_clone());
        b.push(copy);
        let suggestions = find_review_suggestions(
            &a,
            &b,
            &result.matched_functions,
            &unmatched_a,
            &unmatched_b,
            &|| false,
        )
        .unwrap();
        assert!(suggestions.is_empty());

        let (a, mut b) = inputs();
        let changed = b
            .iter_mut()
            .find(|function| function.address == 0x1000004c4)
            .unwrap();
        for instruction in &mut changed.instructions {
            instruction.mnemonic = "unrelated_opcode".into();
        }
        let suggestions = find_review_suggestions(
            &a,
            &b,
            &result.matched_functions,
            &result.unmatched_functions_a,
            &result.unmatched_functions_b,
            &|| false,
        )
        .unwrap();
        assert!(suggestions.is_empty());
    }
}
