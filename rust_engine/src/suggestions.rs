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

    fn json(functions: &[FunctionInfo]) -> String {
        serde_json::to_string(functions).unwrap()
    }

    /// The patched wrapper gains a bounds-check block, which drops its body
    /// score to 0.158. It is still the only unmatched callee of the matched
    /// caller and the only unmatched caller of the matched `__memcpy_chk`, so
    /// call-graph structure must pair it as a changed function rather than
    /// leaving the patch as one removed and one added function.
    #[test]
    fn patched_memcpy_wrapper_is_matched_by_call_graph_structure() {
        let engine = BinaryDiffEngine::new();
        let forward = engine.perform_diff_json(REFERENCE, TARGET).unwrap();
        let reverse = engine.perform_diff_json(TARGET, REFERENCE).unwrap();
        assert_eq!(forward.matched_functions.len(), 5);
        let wrapper = forward
            .matched_functions
            .iter()
            .find(|matched| matched.function_a.address == 0x1000005ac)
            .expect("patched wrapper must be matched");
        assert_eq!(wrapper.function_b.address, 0x1000004c4);
        assert_eq!(wrapper.match_type, crate::MatchType::CallGraph);
        assert_eq!(wrapper.content_match, crate::types::ContentMatch::Changed);
        assert!(wrapper.similarity < 0.35);
        assert!(wrapper.confidence <= 0.5);
        assert!(wrapper.details.neighbor_support >= 2);
        // Vulnerable -> fixed: the added `b.ls` guard sits in front of the
        // unchanged memcpy call.
        let fixed = reverse
            .matched_functions
            .iter()
            .find(|matched| matched.function_b.address == 0x1000005ac)
            .unwrap();
        assert!(fixed.security.evidence.iter().any(|item| {
            item == "added guard in a function with an existing memory operation"
        }));
        assert_eq!(wrapper.details.neighbor_contradictions, 0);
        assert!(forward.review_suggestions.is_empty());
        // `main` differs only in calling `copy_record` by its stripped name
        // and in relocated branch targets.
        let main = forward
            .matched_functions
            .iter()
            .find(|matched| matched.function_a.name == "main")
            .unwrap();
        assert_eq!(main.content_match, crate::types::ContentMatch::Cosmetic);
        // The stripped build's `_main` and the debug build's `main` are one
        // symbol, so `main` is a binding name anchor.
        assert_eq!(main.match_type, crate::MatchType::Name);
        assert!(main.details.binding_anchor);
        assert_eq!(
            main.security.state,
            crate::types::SecurityRelevanceState::Unknown
        );
        assert!(reverse.matched_functions.iter().any(|matched| {
            (matched.function_a.address, matched.function_b.address) == (0x1000004c4, 0x1000005ac)
        }));
    }

    #[test]
    fn competing_or_unrelated_graph_candidates_are_not_forced_or_suggested() {
        // A second unmatched counterpart at the same call sites removes the
        // structural uniqueness that justifies a low-similarity match.
        let (a, mut b) = inputs();
        let mut copy = b
            .iter()
            .find(|function| function.address == 0x1000004c4)
            .unwrap()
            .clone();
        copy.address = 0x100010000;
        copy.name = "sub_100010000".into();
        copy.basic_blocks[0].address = copy.address;
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
        b.push(copy);
        let competing = BinaryDiffEngine::new()
            .perform_diff_json(&json(&a), &json(&b))
            .unwrap();
        assert!(competing
            .matched_functions
            .iter()
            .all(|matched| matched.function_a.address != 0x1000005ac));
        assert!(competing.review_suggestions.is_empty());

        // An unrelated body at the same call-graph position is not forced.
        let (a, mut b) = inputs();
        let changed = b
            .iter_mut()
            .find(|function| function.address == 0x1000004c4)
            .unwrap();
        for instruction in changed.instructions.iter_mut().chain(
            changed
                .basic_blocks
                .iter_mut()
                .flat_map(|block| block.instructions.iter_mut()),
        ) {
            instruction.mnemonic = "unrelated_opcode".into();
        }
        let unrelated = BinaryDiffEngine::new()
            .perform_diff_json(&json(&a), &json(&b))
            .unwrap();
        assert!(unrelated
            .matched_functions
            .iter()
            .all(|matched| matched.function_a.address != 0x1000005ac));
    }

    #[test]
    fn competing_or_unrelated_graph_candidates_are_not_suggested() {
        // Exercise the hint layer directly with the wrapper left unmatched.
        let mut result = BinaryDiffEngine::new()
            .perform_diff_json(REFERENCE, TARGET)
            .unwrap();
        result
            .matched_functions
            .retain(|matched| matched.function_a.address != 0x1000005ac);
        let (a, b) = inputs();
        result.unmatched_functions_a.extend(
            a.iter()
                .filter(|function| function.address == 0x1000005ac)
                .map(FunctionInfo::slimmed_clone),
        );
        result.unmatched_functions_b.extend(
            b.iter()
                .filter(|function| function.address == 0x1000004c4)
                .map(FunctionInfo::slimmed_clone),
        );
        let hints = find_review_suggestions(
            &a,
            &b,
            &result.matched_functions,
            &result.unmatched_functions_a,
            &result.unmatched_functions_b,
            &|| false,
        )
        .unwrap();
        assert_eq!(hints.len(), 1);
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
