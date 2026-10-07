use crate::algorithms::{DiffAlgorithms, FunctionFeatures};
use crate::types::{
    is_generated_function_name, FunctionInfo, FunctionMatch, MatchDetails, MatchType,
};
use anyhow::{bail, Result};
use rayon::prelude::*;
use rustc_hash::{FxHashMap, FxHashSet};
use std::collections::VecDeque;

#[derive(Clone)]
struct Candidate {
    a: usize,
    b: usize,
    similarity: f64,
    pairing_score: f64,
    ambiguity_margin: f64,
    details: MatchDetails,
}

/// Immutable trusted correspondence. Only unique exact/name seeds are binding.
/// Neighbor facts include tentative context, recomputed against each complete
/// snapshot. Tentative context only affects ranking, never reservations. No cross-product
/// of duplicate-key buckets is materialized here.
struct TrustedMatchEvidence {
    anchors_a: FxHashMap<usize, usize>,
    anchors_b: FxHashMap<usize, usize>,
    context_a: FxHashMap<usize, usize>,
    context_b: FxHashMap<usize, usize>,
    callers_a: Vec<FxHashSet<usize>>,
    callers_b: Vec<FxHashSet<usize>>,
    callees_a: Vec<FxHashSet<usize>>,
    callees_b: Vec<FxHashSet<usize>>,
    caller_memberships_b: Vec<Vec<usize>>,
    callee_memberships_b: Vec<Vec<usize>>,
}

impl TrustedMatchEvidence {
    fn new(functions_a: &[FunctionInfo], functions_b: &[FunctionInfo]) -> Self {
        let neighborhoods = |functions: &[FunctionInfo]| {
            let indices: FxHashMap<_, _> = functions
                .iter()
                .enumerate()
                .map(|(i, f)| (f.address, i))
                .collect();
            let callers: Vec<FxHashSet<usize>> = functions
                .iter()
                .map(|f| {
                    f.callers
                        .iter()
                        .filter_map(|a| indices.get(a).copied())
                        .collect()
                })
                .collect();
            let callees: Vec<FxHashSet<usize>> = functions
                .iter()
                .map(|f| {
                    f.callees
                        .iter()
                        .filter_map(|a| indices.get(a).copied())
                        .collect()
                })
                .collect();
            (callers, callees)
        };
        let (callers_a, callees_a) = neighborhoods(functions_a);
        let (callers_b, callees_b) = neighborhoods(functions_b);
        // Invert the B-side neighbor sets exactly as extracted. The callers
        // and callees lists need not be reciprocal in an incomplete analysis.
        let memberships = |neighbors: &[FxHashSet<usize>]| {
            let mut inverse = vec![Vec::new(); neighbors.len()];
            for (function, members) in neighbors.iter().enumerate() {
                for &member in members {
                    inverse[member].push(function);
                }
            }
            inverse
        };
        let caller_memberships_b = memberships(&callers_b);
        let callee_memberships_b = memberships(&callees_b);
        Self {
            anchors_a: FxHashMap::default(),
            anchors_b: FxHashMap::default(),
            context_a: FxHashMap::default(),
            context_b: FxHashMap::default(),
            callers_a,
            callers_b,
            callees_a,
            callees_b,
            caller_memberships_b,
            callee_memberships_b,
        }
    }

    /// Enumerate only pairs with at least one mapped neighbor in common.
    /// A pair without positive support cannot pass the refinement gate.
    fn neighbor_supported_pairs(
        &self,
        group_a: &[usize],
        group_b: &[usize],
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<Vec<(usize, usize)>> {
        if self.anchors_a.is_empty() && self.context_a.is_empty() {
            return Ok(Vec::new());
        }
        let eligible_b: FxHashSet<usize> = group_b.iter().copied().collect();
        let mut pairs = FxHashSet::default();
        for &a in group_a {
            if cancelled() {
                bail!("function matching cancelled");
            }
            for (left, inverse) in [
                (&self.callers_a[a], &self.caller_memberships_b),
                (&self.callees_a[a], &self.callee_memberships_b),
            ] {
                for neighbor in left {
                    if cancelled() {
                        bail!("function matching cancelled");
                    }
                    if let Some(&mapped) = self
                        .context_a
                        .get(neighbor)
                        .or_else(|| self.anchors_a.get(neighbor))
                    {
                        for &b in &inverse[mapped] {
                            if cancelled() {
                                bail!("function matching cancelled");
                            }
                            if eligible_b.contains(&b) {
                                pairs.insert((a, b));
                            }
                        }
                    }
                }
            }
        }
        let mut pairs: Vec<_> = pairs.into_iter().collect();
        pairs.sort_unstable();
        Ok(pairs)
    }

    fn seed_unique<K: std::hash::Hash + Eq>(
        &mut self,
        keys_a: &[Option<K>],
        keys_b: &[Option<K>],
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<()> {
        // Count the complete bucket, including already reserved endpoints.
        // Removing a conflict must not manufacture a new unique name proof.
        let buckets = |keys: &[Option<K>]| -> Result<FxHashMap<usize, usize>> {
            let mut by_key: FxHashMap<&K, (usize, usize)> = FxHashMap::default();
            for (index, key) in keys.iter().enumerate() {
                if cancelled() {
                    bail!("function matching cancelled");
                }
                if let Some(key) = key {
                    let entry = by_key.entry(key).or_insert((index, 0));
                    entry.1 += 1;
                }
            }
            Ok(by_key.into_values().filter(|(_, n)| *n == 1).collect())
        };
        let unique_a = buckets(keys_a)?;
        let unique_b = buckets(keys_b)?;
        let by_key_b: FxHashMap<&K, usize> = unique_b
            .keys()
            .map(|&i| (keys_b[i].as_ref().unwrap(), i))
            .collect();
        for (a, key) in keys_a.iter().enumerate() {
            if cancelled() {
                bail!("function matching cancelled");
            }
            if !unique_a.contains_key(&a) || self.anchors_a.contains_key(&a) {
                continue;
            }
            if let Some(&b) = by_key_b.get(key.as_ref().unwrap()) {
                if !self.anchors_b.contains_key(&b) {
                    self.anchors_a.insert(a, b);
                    self.anchors_b.insert(b, a);
                }
            }
        }
        Ok(())
    }

    fn allows(&self, a: usize, b: usize) -> bool {
        self.anchors_a.get(&a).is_none_or(|&mapped| mapped == b)
            && self.anchors_b.get(&b).is_none_or(|&mapped| mapped == a)
    }

    fn neighbor_counts(&self, a: usize, b: usize) -> (usize, usize) {
        let (mut support, mut contradiction) = (0, 0);
        for (left, right) in [
            (&self.callers_a[a], &self.callers_b[b]),
            (&self.callees_a[a], &self.callees_b[b]),
        ] {
            for neighbor in left {
                if let Some(mapped) = self
                    .context_a
                    .get(neighbor)
                    .or_else(|| self.anchors_a.get(neighbor))
                {
                    if right.contains(mapped) {
                        support += 1;
                    } else {
                        contradiction += 1;
                    }
                }
            }
            for neighbor in right {
                if let Some(mapped) = self
                    .context_b
                    .get(neighbor)
                    .or_else(|| self.anchors_b.get(neighbor))
                {
                    if !left.contains(mapped) {
                        contradiction += 1;
                    }
                }
            }
        }
        (support, contradiction)
    }

    // Prune against a complete snapshot, including loss of an anchored support
    // path. Removal can invalidate dependents, so repeat to a decreasing closure.
    fn prune_context(
        &mut self,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<Vec<(usize, usize)>> {
        let mut removed = Vec::new();
        loop {
            let mut reachable: FxHashSet<usize> = self.anchors_a.keys().copied().collect();
            let mut queue: VecDeque<usize> = reachable.iter().copied().collect();
            while let Some(a) = queue.pop_front() {
                if cancelled() {
                    bail!("function matching cancelled");
                }
                let b = self
                    .anchors_a
                    .get(&a)
                    .or_else(|| self.context_a.get(&a))
                    .unwrap();
                for (left, right) in [
                    (&self.callers_a[a], &self.callers_b[*b]),
                    (&self.callees_a[a], &self.callees_b[*b]),
                ] {
                    for &next in left {
                        if self
                            .context_a
                            .get(&next)
                            .is_some_and(|mapped| right.contains(mapped))
                            && reachable.insert(next)
                        {
                            queue.push_back(next);
                        }
                    }
                }
            }
            let invalid: Vec<_> = self
                .context_a
                .iter()
                .filter_map(|(&a, &b)| {
                    let (positive, negative) = self.neighbor_counts(a, b);
                    (!reachable.contains(&a) || positive <= negative).then_some((a, b))
                })
                .collect();
            if invalid.is_empty() {
                break;
            }
            for (a, b) in invalid {
                self.context_a.remove(&a);
                self.context_b.remove(&b);
                removed.push((a, b));
            }
        }
        Ok(removed)
    }

    fn pairing_score(&self, a: usize, b: usize, body_score: f64) -> f64 {
        let (positive, negative) = self.neighbor_counts(a, b);
        let neighborhood = if positive + negative == 0 {
            0.5
        } else {
            positive as f64 / (positive + negative) as f64
        };
        (0.98 * body_score + 0.02 * neighborhood).clamp(0.0, 1.0)
    }
}

// Rank the entire equal-size run before limiting expensive comparisons. The
// address tie-break makes retrieval invariant to extraction order.
fn shortlist(
    function: &FunctionInfo,
    features: &FunctionFeatures,
    others: &[FunctionInfo],
    other_features: &[FunctionFeatures],
    by_size: &[(usize, usize)],
) -> Vec<usize> {
    const WINDOW: usize = 256;
    const LIMIT: usize = 48;
    let count = function.instructions.len();
    let first = by_size.partition_point(|&(_, size)| size < count);
    let last = by_size.partition_point(|&(_, size)| size <= count);
    let start = first.saturating_sub(WINDOW);
    let end = last.saturating_add(WINDOW).min(by_size.len());
    let mut ranked: Vec<_> = by_size[start..end]
        .iter()
        .map(|&(i, size)| {
            (
                i,
                DiffAlgorithms::retrieval_similarity(features, &other_features[i]),
                count.abs_diff(size),
                function
                    .basic_blocks
                    .len()
                    .abs_diff(others[i].basic_blocks.len()),
                others[i].address,
            )
        })
        .collect();
    let compare = |a: &(usize, f64, usize, usize, u64), b: &(usize, f64, usize, usize, u64)| {
        b.1.total_cmp(&a.1)
            .then(a.2.cmp(&b.2))
            .then(a.3.cmp(&b.3))
            .then(a.4.cmp(&b.4))
    };
    if ranked.len() > LIMIT {
        ranked.select_nth_unstable_by(LIMIT, compare);
        ranked.truncate(LIMIT);
    }
    ranked.sort_unstable_by(compare);
    ranked.into_iter().map(|item| item.0).collect()
}

/// A second retrieval route for functions that moved beyond the size window.
/// Only strings seen in at most four unmatched functions can nominate pairs;
/// final body-similarity and assignment gates remain unchanged.
struct RareStringIndex<'a> {
    postings: FxHashMap<&'a str, Vec<usize>>,
}

impl<'a> RareStringIndex<'a> {
    fn new(features: &'a [FunctionFeatures], eligible: &[(usize, usize)]) -> Self {
        const MAX_OCCURRENCES: usize = 4;
        let mut postings: FxHashMap<&str, Vec<usize>> = FxHashMap::default();
        for &(index, _) in eligible {
            for value in features[index].retrieval_strings() {
                // Very short literals are poor identity evidence even when
                // they happen to be rare in a particular pair.
                if value.len() < 6 {
                    continue;
                }
                let entries = postings.entry(value.as_str()).or_default();
                if entries.len() <= MAX_OCCURRENCES {
                    entries.push(index);
                }
            }
        }
        Self { postings }
    }

    fn shortlist(
        &self,
        function: &FunctionInfo,
        features: &FunctionFeatures,
        others: &[FunctionInfo],
        other_features: &[FunctionFeatures],
    ) -> Vec<usize> {
        const MAX_OCCURRENCES: usize = 4;
        const LIMIT: usize = 48;
        const MIN_CONTENT_OVERLAP: f64 = 0.5;
        let mut votes: FxHashMap<usize, usize> = FxHashMap::default();
        for value in features.retrieval_strings() {
            if let Some(indices) = self.postings.get(value.as_str()) {
                if indices.len() <= MAX_OCCURRENCES {
                    for &index in indices {
                        *votes.entry(index).or_insert(0) += 1;
                    }
                }
            }
        }
        let mut ranked: Vec<_> = votes
            .into_iter()
            .filter_map(|(index, votes)| {
                let content =
                    DiffAlgorithms::instruction_content_overlap(features, &other_features[index]);
                (content >= MIN_CONTENT_OVERLAP).then(|| {
                    (
                        index,
                        votes,
                        DiffAlgorithms::retrieval_similarity(features, &other_features[index]),
                        function
                            .instructions
                            .len()
                            .abs_diff(others[index].instructions.len()),
                        others[index].address,
                    )
                })
            })
            .collect();
        ranked.sort_unstable_by(|a, b| {
            b.1.cmp(&a.1)
                .then(b.2.total_cmp(&a.2))
                .then(a.3.cmp(&b.3))
                .then(a.4.cmp(&b.4))
        });
        ranked.truncate(LIMIT);
        ranked.into_iter().map(|item| item.0).collect()
    }
}

fn greedy_candidates(mut candidates: Vec<Candidate>) -> Vec<Candidate> {
    candidates.sort_by(|left, right| {
        right
            .pairing_score
            .total_cmp(&left.pairing_score)
            .then_with(|| right.similarity.total_cmp(&left.similarity))
            .then_with(|| left.a.cmp(&right.a))
            .then_with(|| left.b.cmp(&right.b))
    });
    let mut used_a = FxHashSet::default();
    let mut used_b = FxHashSet::default();
    candidates
        .into_iter()
        .filter(|candidate| {
            if used_a.contains(&candidate.a) || used_b.contains(&candidate.b) {
                false
            } else {
                used_a.insert(candidate.a);
                used_b.insert(candidate.b);
                true
            }
        })
        .collect()
}

fn annotate_ambiguity_margins(candidates: &mut [Candidate]) {
    #[derive(Clone, Copy)]
    struct TopTwo {
        first_index: usize,
        first_score: f64,
        second_score: f64,
    }
    impl Default for TopTwo {
        fn default() -> Self {
            Self {
                first_index: usize::MAX,
                first_score: f64::NEG_INFINITY,
                second_score: f64::NEG_INFINITY,
            }
        }
    }
    let update = |top: &mut TopTwo, index: usize, score: f64| {
        if score > top.first_score {
            top.second_score = top.first_score;
            top.first_score = score;
            top.first_index = index;
        } else if index != top.first_index && score > top.second_score {
            top.second_score = score;
        }
    };
    let mut by_a: FxHashMap<usize, TopTwo> = FxHashMap::default();
    let mut by_b: FxHashMap<usize, TopTwo> = FxHashMap::default();
    for (index, candidate) in candidates.iter().enumerate() {
        update(
            by_a.entry(candidate.a).or_default(),
            index,
            candidate.pairing_score,
        );
        update(
            by_b.entry(candidate.b).or_default(),
            index,
            candidate.pairing_score,
        );
    }
    for (index, candidate) in candidates.iter_mut().enumerate() {
        let alternative_for = |top: &TopTwo| {
            if top.first_index == index {
                top.second_score
            } else {
                top.first_score
            }
        };
        let alternative = [
            alternative_for(&by_a[&candidate.a]),
            alternative_for(&by_b[&candidate.b]),
        ]
        .into_iter()
        .filter(|score| score.is_finite())
        .max_by(f64::total_cmp);
        candidate.ambiguity_margin = alternative
            .map(|score| (candidate.pairing_score - score).max(0.0))
            .unwrap_or(1.0);
    }
}

/// Maximum-cardinality, maximum-weight assignment for one connected ambiguity
/// component. Qualified candidate edges outrank leaving a row unmatched; the
/// pairing score then chooses the best complete assignment. Large pathological
/// components retain a deterministic greedy fallback.
fn optimal_component(candidates: Vec<Candidate>) -> Vec<Candidate> {
    const MAX_OPTIMAL_SIDE: usize = 128;
    let mut side_a: Vec<usize> = candidates.iter().map(|candidate| candidate.a).collect();
    let mut side_b: Vec<usize> = candidates.iter().map(|candidate| candidate.b).collect();
    side_a.sort_unstable();
    side_a.dedup();
    side_b.sort_unstable();
    side_b.dedup();
    if side_a.len().max(side_b.len()) > MAX_OPTIMAL_SIDE {
        return greedy_candidates(candidates);
    }

    let index_a: FxHashMap<usize, usize> = side_a
        .iter()
        .copied()
        .enumerate()
        .map(|(index, value)| (value, index))
        .collect();
    let index_b: FxHashMap<usize, usize> = side_b
        .iter()
        .copied()
        .enumerate()
        .map(|(index, value)| (value, index))
        .collect();
    let rows = side_a.len();
    let real_columns = side_b.len();
    let columns = real_columns + rows;
    const MISSING: f64 = 1_000_000.0;
    let mut costs = vec![vec![MISSING; columns]; rows];
    for row in &mut costs {
        row[real_columns..].fill(0.0);
    }
    // Pairing scores are in [0, 1]. A bonus larger than the maximum number of
    // real pairs makes one additional qualified edge worth more than every
    // possible score difference, giving cardinality strict priority.
    let cardinality_bonus = rows.min(real_columns) as f64 + 1.0;
    for candidate in &candidates {
        costs[index_a[&candidate.a]][index_b[&candidate.b]] =
            -(cardinality_bonus + candidate.pairing_score);
    }

    // Rectangular Hungarian algorithm for minimum cost (rows <= columns).
    let mut u = vec![0.0; rows + 1];
    let mut v = vec![0.0; columns + 1];
    let mut p = vec![0usize; columns + 1];
    let mut way = vec![0usize; columns + 1];
    for row in 1..=rows {
        p[0] = row;
        let mut column0 = 0usize;
        let mut minv = vec![f64::INFINITY; columns + 1];
        let mut used = vec![false; columns + 1];
        loop {
            used[column0] = true;
            let row0 = p[column0];
            let mut delta = f64::INFINITY;
            let mut column1 = 0usize;
            for column in 1..=columns {
                if used[column] {
                    continue;
                }
                let current = costs[row0 - 1][column - 1] - u[row0] - v[column];
                if current < minv[column] {
                    minv[column] = current;
                    way[column] = column0;
                }
                if minv[column] < delta {
                    delta = minv[column];
                    column1 = column;
                }
            }
            for column in 0..=columns {
                if used[column] {
                    u[p[column]] += delta;
                    v[column] -= delta;
                } else {
                    minv[column] -= delta;
                }
            }
            column0 = column1;
            if p[column0] == 0 {
                break;
            }
        }
        loop {
            let previous = way[column0];
            p[column0] = p[previous];
            column0 = previous;
            if column0 == 0 {
                break;
            }
        }
    }

    let mut selected = Vec::new();
    for column in 1..=real_columns {
        let row = p[column];
        if row == 0 || costs[row - 1][column - 1] >= MISSING {
            continue;
        }
        let a = side_a[row - 1];
        let b = side_b[column - 1];
        if let Some(candidate) = candidates
            .iter()
            .find(|candidate| candidate.a == a && candidate.b == b)
        {
            selected.push(candidate.clone());
        }
    }
    selected.sort_by_key(|candidate| (candidate.a, candidate.b));
    selected
}

/// Split the candidate graph into independent ambiguity components before
/// solving. This keeps the optimal path cheap for the common case of small
/// duplicate buckets.
fn assign_candidates(mut candidates: Vec<Candidate>) -> Vec<Candidate> {
    annotate_ambiguity_margins(&mut candidates);
    let mut by_a: FxHashMap<usize, Vec<usize>> = FxHashMap::default();
    let mut by_b: FxHashMap<usize, Vec<usize>> = FxHashMap::default();
    for (index, candidate) in candidates.iter().enumerate() {
        by_a.entry(candidate.a).or_default().push(index);
        by_b.entry(candidate.b).or_default().push(index);
    }
    let mut visited = vec![false; candidates.len()];
    let mut selected = Vec::new();
    for seed in 0..candidates.len() {
        if visited[seed] {
            continue;
        }
        let mut queue = VecDeque::from([seed]);
        let mut component = Vec::new();
        visited[seed] = true;
        while let Some(index) = queue.pop_front() {
            let candidate = &candidates[index];
            component.push(candidate.clone());
            for neighbor in by_a[&candidate.a].iter().chain(by_b[&candidate.b].iter()) {
                if !visited[*neighbor] {
                    visited[*neighbor] = true;
                    queue.push_back(*neighbor);
                }
            }
        }
        selected.extend(optimal_component(component));
    }
    selected.sort_by_key(|candidate| (candidate.a, candidate.b));
    selected
}

/// Returns true if `name` looks like an auto-generated placeholder. Keep this
/// policy in sync with the native symbol-port planner: prefixes alone are not
/// enough because an analyst name such as `sub_parse_header` is meaningful.
fn is_auto_generated_name(name: &str) -> bool {
    is_generated_function_name(name)
}

/// Pair-independent matching features, prepared once for a retained snapshot.
/// Only unsorted inputs need a canonical copy; original order is kept for exports.
pub(crate) struct PreparedMatching {
    ordered: Option<Vec<FunctionInfo>>,
    features: Vec<FunctionFeatures>,
    md: Vec<Option<String>>,
    exact: Vec<Option<(String, String, String)>>,
}

impl PreparedMatching {
    pub(crate) fn new(
        functions: &[FunctionInfo],
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<Self> {
        if cancelled() {
            bail!("function preparation cancelled");
        }
        let ordered = if functions.windows(2).all(|w| w[0].address <= w[1].address) {
            None
        } else {
            let mut ordered = Vec::with_capacity(functions.len());
            for function in functions {
                if cancelled() {
                    bail!("function preparation cancelled");
                }
                ordered.push(function.clone());
            }
            ordered.sort_by_key(|f| f.address);
            Some(ordered)
        };
        let source = ordered.as_deref().unwrap_or(functions);
        let features = source
            .par_iter()
            .map(|function| {
                if cancelled() {
                    bail!("function preparation cancelled");
                }
                Ok(DiffAlgorithms::function_features(function))
            })
            .collect::<Result<Vec<_>>>()?;
        let md = source
            .par_iter()
            .map(|f| {
                if cancelled() {
                    bail!("function preparation cancelled");
                }
                Ok((!f.basic_blocks.is_empty() && !f.instructions.is_empty())
                    .then(|| DiffAlgorithms::calculate_md_index(f)))
            })
            .collect::<Result<Vec<_>>>()?;
        let exact = source
            .par_iter()
            .map(|f| {
                if cancelled() {
                    bail!("function preparation cancelled");
                }
                Ok((!f.instructions.is_empty()).then(|| {
                    (
                        DiffAlgorithms::calculate_instruction_content_hash(f),
                        f.cfg_hash.clone(),
                        f.call_graph_hash.clone(),
                    )
                }))
            })
            .collect::<Result<Vec<_>>>()?;
        Ok(Self {
            ordered,
            features,
            md,
            exact,
        })
    }

    fn functions<'a>(&'a self, original: &'a [FunctionInfo]) -> &'a [FunctionInfo] {
        self.ordered.as_deref().unwrap_or(original)
    }

    pub(crate) fn estimated_heap_bytes(&self) -> usize {
        self.ordered.as_ref().map_or(0, |fs| {
            fs.capacity() * std::mem::size_of::<FunctionInfo>()
                + fs.iter()
                    .map(FunctionInfo::estimated_heap_bytes)
                    .sum::<usize>()
        }) + self.features.capacity() * std::mem::size_of::<FunctionFeatures>()
            + self
                .features
                .iter()
                .map(FunctionFeatures::estimated_heap_bytes)
                .sum::<usize>()
            + self.md.capacity() * std::mem::size_of::<Option<String>>()
            + self
                .md
                .iter()
                .flatten()
                .map(String::capacity)
                .sum::<usize>()
            + self.exact.capacity() * std::mem::size_of::<Option<(String, String, String)>>()
            + self
                .exact
                .iter()
                .flatten()
                .map(|(a, b, c)| a.capacity() + b.capacity() + c.capacity())
                .sum::<usize>()
    }
}

pub struct MatchingEngine {
    confidence_threshold: f64,
    similarity_threshold: f64,
}

impl Default for MatchingEngine {
    fn default() -> Self {
        Self::new()
    }
}

impl MatchingEngine {
    pub fn new() -> Self {
        Self {
            confidence_threshold: 0.5,
            similarity_threshold: 0.6,
        }
    }

    pub fn with_thresholds(confidence: f64, similarity: f64) -> Self {
        Self {
            confidence_threshold: confidence,
            similarity_threshold: similarity,
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn push_match(
        matches: &mut Vec<FunctionMatch>,
        used_a: &mut FxHashSet<usize>,
        used_b: &mut FxHashSet<usize>,
        functions_a: &[FunctionInfo],
        functions_b: &[FunctionInfo],
        idx_a: usize,
        idx_b: usize,
        similarity: f64,
        ambiguity_margin: f64,
        details: MatchDetails,
        match_type: MatchType,
    ) {
        let confidence = DiffAlgorithms::confidence_for_match(&match_type, similarity);
        let content_match =
            crate::algorithms::classify_content_match(&functions_a[idx_a], &functions_b[idx_b]);
        matches.push(FunctionMatch {
            function_a: functions_a[idx_a].slimmed_clone(),
            function_b: functions_b[idx_b].slimmed_clone(),
            similarity,
            confidence,
            ambiguity_margin,
            match_type,
            content_match,
            details,
            security: Default::default(),
        });
        used_a.insert(idx_a);
        used_b.insert(idx_b);
    }

    /// Primary matching function using native heuristics, ordered from
    /// strongest evidence to weakest.
    pub fn match_functions(
        &self,
        functions_a: &[FunctionInfo],
        functions_b: &[FunctionInfo],
    ) -> Result<Vec<FunctionMatch>> {
        self.match_functions_cancellable(functions_a, functions_b, &|| false)
    }

    pub fn match_functions_cancellable(
        &self,
        functions_a: &[FunctionInfo],
        functions_b: &[FunctionInfo],
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<Vec<FunctionMatch>> {
        if cancelled() {
            bail!("function matching cancelled");
        }
        let prepared_a = PreparedMatching::new(functions_a, cancelled)?;
        let prepared_b = PreparedMatching::new(functions_b, cancelled)?;
        self.match_prepared(
            functions_a,
            &prepared_a,
            functions_b,
            &prepared_b,
            cancelled,
        )
    }

    pub(crate) fn match_prepared(
        &self,
        original_a: &[FunctionInfo],
        prepared_a: &PreparedMatching,
        original_b: &[FunctionInfo],
        prepared_b: &PreparedMatching,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<Vec<FunctionMatch>> {
        if cancelled() {
            bail!("function matching cancelled");
        }
        let functions_a = prepared_a.functions(original_a);
        let functions_b = prepared_b.functions(original_b);
        let features_a = &prepared_a.features;
        let features_b = &prepared_b.features;
        let mut matches = Vec::new();
        let mut used_a = FxHashSet::default();
        let mut used_b = FxHashSet::default();

        // Minimum sizes below which a phase's key carries no signal.
        const MIN_SPP_INSTRUCTIONS: usize = 5;

        // Precompute per-function keys once instead of per-pair.
        let prime_table = DiffAlgorithms::build_mnemonic_prime_table(functions_a, functions_b);
        let md_a = &prepared_a.md;
        let md_b = &prepared_b.md;
        let spp_key = |f: &FunctionInfo| {
            (f.instructions.len() >= MIN_SPP_INSTRUCTIONS)
                .then(|| DiffAlgorithms::calculate_small_primes_product(f, &prime_table))
        };
        let spp_a: Vec<Option<u64>> = functions_a.par_iter().map(spp_key).collect();
        let spp_b: Vec<Option<u64>> = functions_b.par_iter().map(spp_key).collect();

        let exact_a = &prepared_a.exact;
        let exact_b = &prepared_b.exact;

        fn wl_key(f: &FunctionInfo) -> Option<&str> {
            const MIN_STRUCTURAL_BLOCKS: usize = 3;
            (f.basic_blocks.len() >= MIN_STRUCTURAL_BLOCKS).then_some(f.cfg_hash.as_str())
        }
        let wl_a: Vec<Option<&str>> = functions_a.iter().map(wl_key).collect();
        let wl_b: Vec<Option<&str>> = functions_b.iter().map(wl_key).collect();
        fn meaningful_name(function: &FunctionInfo) -> Option<&str> {
            (!is_auto_generated_name(&function.name)).then_some(function.name.as_str())
        }
        let names_a: Vec<Option<&str>> = functions_a.iter().map(meaningful_name).collect();
        let names_b: Vec<Option<&str>> = functions_b.iter().map(meaningful_name).collect();

        let mut evidence = TrustedMatchEvidence::new(functions_a, functions_b);
        evidence.seed_unique(&exact_a, &exact_b, cancelled)?;
        evidence.seed_unique(&names_a, &names_b, cancelled)?;
        // Recover neighborhood discrimination through tentative correspondences.
        // These influence ranking only; they never reserve an endpoint or become
        // binding identity evidence. Recompute facts against each full snapshot.
        // Retain buckets, and enumerate only pairs with a mapped neighbor in
        // common during refinement.
        let mut groups: Vec<(Vec<usize>, Vec<usize>)> = Vec::new();
        macro_rules! join_keys {
            ($left:expr, $right:expr) => {{
                let (mut left, mut right) = (FxHashMap::default(), FxHashMap::default());
                for (a, key) in $left.iter().enumerate() {
                    if cancelled() {
                        bail!("function matching cancelled");
                    }
                    if let Some(key) = key {
                        left.entry(key).or_insert_with(Vec::new).push(a);
                    }
                }
                for (b, key) in $right.iter().enumerate() {
                    if cancelled() {
                        bail!("function matching cancelled");
                    }
                    if let Some(key) = key {
                        right.entry(key).or_insert_with(Vec::new).push(b);
                    }
                }
                for (key, aa) in left {
                    if let Some(bb) = right.remove(key) {
                        groups.push((aa, bb));
                    }
                }
            }};
        }
        join_keys!(&exact_a, &exact_b);
        join_keys!(&names_a, &names_b);
        join_keys!(&wl_a, &wl_b);
        join_keys!(&md_a, &md_b);
        join_keys!(&spp_a, &spp_b);
        let mut rejected_context = FxHashSet::default();
        loop {
            let mut supported = FxHashSet::default();
            let (mut degrees_a, mut degrees_b) = (FxHashMap::default(), FxHashMap::default());
            for (aa, bb) in &groups {
                for (a, b) in evidence.neighbor_supported_pairs(aa, bb, cancelled)? {
                    if cancelled() {
                        bail!("function matching cancelled");
                    }
                    if rejected_context.contains(&(a, b))
                        || evidence.anchors_a.contains_key(&a)
                        || evidence.anchors_b.contains_key(&b)
                        || evidence.context_a.contains_key(&a)
                        || evidence.context_b.contains_key(&b)
                    {
                        continue;
                    }
                    let (positive, negative) = evidence.neighbor_counts(a, b);
                    if positive == 0 || positive <= negative {
                        continue;
                    }
                    let exact_or_name = (exact_a[a].is_some() && exact_a[a] == exact_b[b])
                        || (names_a[a].is_some() && names_a[a] == names_b[b]);
                    if !exact_or_name {
                        let (similarity, _) = DiffAlgorithms::compute_match_details_with_features(
                            &functions_a[a],
                            &functions_b[b],
                            &features_a[a],
                            &features_b[b],
                        );
                        let keyed = (md_a[a].is_some() && md_a[a] == md_b[b])
                            || (spp_a[a].is_some() && spp_a[a] == spp_b[b]);
                        let threshold = if keyed {
                            self.similarity_threshold
                        } else {
                            (self.similarity_threshold - 0.2).max(0.3)
                        };
                        if similarity < threshold {
                            continue;
                        }
                    }
                    if !supported.insert((a, b)) {
                        continue;
                    }
                    *degrees_a.entry(a).or_insert(0usize) += 1;
                    *degrees_b.entry(b).or_insert(0usize) += 1;
                }
            }
            let mut added = false;
            for (a, b) in supported {
                if degrees_a[&a] == 1 && degrees_b[&b] == 1 {
                    evidence.context_a.insert(a, b);
                    evidence.context_b.insert(b, a);
                    added = true;
                }
            }
            rejected_context.extend(evidence.prune_context(cancelled)?);
            if !added {
                break;
            }
        }
        // Assignment may reject tentative context. Retry using only surviving
        // context; this loop strictly decreases the context set on every retry.
        loop {
            matches.clear();
            used_a.clear();
            used_b.clear();
            // Reserve binding seeds before any ambiguous phase can consume them.
            let mut anchors: Vec<_> = evidence.anchors_a.iter().map(|(&a, &b)| (a, b)).collect();
            anchors.sort_unstable();
            for (a, b) in anchors {
                if cancelled() {
                    bail!("function matching cancelled");
                }
                let (similarity, details) = DiffAlgorithms::compute_match_details_with_features(
                    &functions_a[a],
                    &functions_b[b],
                    &features_a[a],
                    &features_b[b],
                );
                let kind = if exact_a[a].is_some() && exact_a[a] == exact_b[b] {
                    MatchType::Exact
                } else {
                    MatchType::Name
                };
                Self::push_match(
                    &mut matches,
                    &mut used_a,
                    &mut used_b,
                    functions_a,
                    functions_b,
                    a,
                    b,
                    similarity,
                    1.0,
                    details,
                    kind,
                );
            }

            // 1. Exact matching: WL CFG hash + call-graph degree + instruction
            //    content hash. Structure alone is not enough for "exact" — small
            //    leaf functions with different instructions share WL hashes.
            if !self.keyed_matching(
                functions_a,
                functions_b,
                &features_a,
                &features_b,
                &exact_a,
                &exact_b,
                MatchType::Exact,
                0.0,
                &evidence,
                &mut matches,
                &mut used_a,
                &mut used_b,
                cancelled,
            ) {
                bail!("function matching cancelled");
            }

            // 2. Symbol name matching (near ground truth for real symbols)
            self.name_matching(
                functions_a,
                functions_b,
                &features_a,
                &features_b,
                &evidence,
                &mut matches,
                &mut used_a,
                &mut used_b,
                cancelled,
            )?;

            // 3. MD-Index matching (topological fingerprint)
            if !self.keyed_matching(
                functions_a,
                functions_b,
                &features_a,
                &features_b,
                &md_a,
                &md_b,
                MatchType::MdIndex,
                self.similarity_threshold,
                &evidence,
                &mut matches,
                &mut used_a,
                &mut used_b,
                cancelled,
            ) {
                bail!("function matching cancelled");
            }

            // 4. Small primes product matching (order-independent instruction multiset)
            if !self.keyed_matching(
                functions_a,
                functions_b,
                &features_a,
                &features_b,
                &spp_a,
                &spp_b,
                MatchType::SmallPrimes,
                self.similarity_threshold,
                &evidence,
                &mut matches,
                &mut used_a,
                &mut used_b,
                cancelled,
            ) {
                bail!("function matching cancelled");
            }

            // 5. Structural matching (WL hash equality; instructions may differ
            //    freely). Trivial CFGs share WL hashes vacuously, so require a
            //    minimum number of blocks for the hash to mean anything.
            if !self.keyed_matching(
                functions_a,
                functions_b,
                &features_a,
                &features_b,
                &wl_a,
                &wl_b,
                MatchType::Structural,
                (self.similarity_threshold - 0.2).max(0.3),
                &evidence,
                &mut matches,
                &mut used_a,
                &mut used_b,
                cancelled,
            ) {
                bail!("function matching cancelled");
            }

            // 6. Match neighbors of binding anchors, without recursive promotion.
            if !self.call_graph_propagation(
                functions_a,
                functions_b,
                &features_a,
                &features_b,
                &evidence,
                &mut matches,
                &mut used_a,
                &mut used_b,
                cancelled,
            ) {
                bail!("function matching cancelled");
            }

            // 7. Fuzzy matching for whatever survives propagation
            self.fuzzy_matching(
                functions_a,
                functions_b,
                &features_a,
                &features_b,
                &evidence,
                &mut matches,
                &mut used_a,
                &mut used_b,
                cancelled,
            )?;

            let indices_a: FxHashMap<_, _> = functions_a
                .iter()
                .enumerate()
                .map(|(i, f)| (f.address, i))
                .collect();
            let indices_b: FxHashMap<_, _> = functions_b
                .iter()
                .enumerate()
                .map(|(i, f)| (f.address, i))
                .collect();
            let assigned: FxHashMap<_, _> = matches
                .iter()
                .map(|m| {
                    (
                        indices_a[&m.function_a.address],
                        indices_b[&m.function_b.address],
                    )
                })
                .collect();
            let rejected: Vec<_> = evidence
                .context_a
                .iter()
                .filter_map(|(&a, &b)| (assigned.get(&a) != Some(&b)).then_some((a, b)))
                .collect();
            if !rejected.is_empty() {
                for (a, b) in rejected {
                    evidence.context_a.remove(&a);
                    evidence.context_b.remove(&b);
                }
                evidence.prune_context(cancelled)?;
                continue;
            }
            for matched in &mut matches {
                let a = indices_a[&matched.function_a.address];
                let b = indices_b[&matched.function_b.address];
                let (support, contradictions) = evidence.neighbor_counts(a, b);
                matched.details.neighbor_support = support;
                matched.details.neighbor_contradictions = contradictions;
                matched.details.binding_anchor = evidence.anchors_a.get(&a) == Some(&b);
                // Exact body equality does not establish identity between duplicate
                // bodies. Keep the method score but do not report certainty on ties.
                if !matched.details.binding_anchor && matched.ambiguity_margin < 0.01 {
                    matched.confidence = matched.confidence.min(0.5);
                }
            }
            return Ok(matches);
        }
    }

    /// Name-based matching for functions with identical real (non-placeholder)
    /// symbols. A shared real symbol is near ground truth, so the similarity
    /// gate is deliberately loose: similarity indicates how much the function
    /// *changed*, it should not decide whether the match exists.
    #[allow(clippy::too_many_arguments)]
    fn name_matching(
        &self,
        functions_a: &[FunctionInfo],
        functions_b: &[FunctionInfo],
        features_a: &[FunctionFeatures],
        features_b: &[FunctionFeatures],
        evidence: &TrustedMatchEvidence,
        matches: &mut Vec<FunctionMatch>,
        used_a: &mut FxHashSet<usize>,
        used_b: &mut FxHashSet<usize>,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<()> {
        let mut name_map_b: FxHashMap<&str, Vec<usize>> = FxHashMap::default();

        for (i, func_b) in functions_b.iter().enumerate() {
            if !used_b.contains(&i) && !is_auto_generated_name(&func_b.name) {
                name_map_b.entry(func_b.name.as_str()).or_default().push(i);
            }
        }

        let mut scored = Vec::new();
        for (idx_a, func_a) in functions_a.iter().enumerate() {
            if cancelled() {
                bail!("function matching cancelled");
            }
            if used_a.contains(&idx_a) || is_auto_generated_name(&func_a.name) {
                continue;
            }
            if let Some(candidates) = name_map_b.get(func_a.name.as_str()) {
                for &idx in candidates {
                    if cancelled() {
                        bail!("function matching cancelled");
                    }
                    if !used_b.contains(&idx) && evidence.allows(idx_a, idx) {
                        let (similarity, details) =
                            DiffAlgorithms::compute_match_details_with_features(
                                func_a,
                                &functions_b[idx],
                                &features_a[idx_a],
                                &features_b[idx],
                            );
                        let body_pairing_score = DiffAlgorithms::pairing_score(
                            func_a,
                            &functions_b[idx],
                            similarity,
                            &details,
                        );
                        let pairing_score = evidence.pairing_score(idx_a, idx, body_pairing_score);
                        scored.push(Candidate {
                            a: idx_a,
                            b: idx,
                            similarity,
                            pairing_score,
                            ambiguity_margin: 0.0,
                            details,
                        });
                    }
                }
            }
        }
        for candidate in assign_candidates(scored) {
            if !used_a.contains(&candidate.a) && !used_b.contains(&candidate.b) {
                Self::push_match(
                    matches,
                    used_a,
                    used_b,
                    functions_a,
                    functions_b,
                    candidate.a,
                    candidate.b,
                    candidate.similarity,
                    candidate.ambiguity_margin,
                    candidate.details,
                    MatchType::Name,
                );
            }
        }

        Ok(())
    }

    /// Generic bucketed matching: functions sharing a precomputed key are
    /// candidates; the best by similarity above `min_similarity` wins.
    /// `None` keys mark functions ineligible for the phase (too small for
    /// the key to carry any signal — degenerate buckets would otherwise
    /// pair unrelated leaf functions).
    /// Used for the exact, MD-Index, small-primes-product, and WL-structural phases.
    #[allow(clippy::too_many_arguments)]
    fn keyed_matching<K: std::hash::Hash + Eq>(
        &self,
        functions_a: &[FunctionInfo],
        functions_b: &[FunctionInfo],
        features_a: &[FunctionFeatures],
        features_b: &[FunctionFeatures],
        keys_a: &[Option<K>],
        keys_b: &[Option<K>],
        match_type: MatchType,
        min_similarity: f64,
        evidence: &TrustedMatchEvidence,
        matches: &mut Vec<FunctionMatch>,
        used_a: &mut FxHashSet<usize>,
        used_b: &mut FxHashSet<usize>,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> bool {
        let mut key_map_b: FxHashMap<&K, Vec<usize>> = FxHashMap::default();
        for (i, key) in keys_b.iter().enumerate() {
            if let (Some(key), false) = (key, used_b.contains(&i)) {
                key_map_b.entry(key).or_default().push(i);
            }
        }

        // Score the full eligible bucket before assigning. Choosing the best B
        // separately for each A made results depend on extraction order: an early
        // ambiguous A could consume the only strong counterpart of a later A.
        // Global best-first assignment preserves determinism while removing that
        // directional bias.
        let mut scored = Vec::new();
        for (idx_a, func_a) in functions_a.iter().enumerate() {
            if cancelled() {
                return false;
            }
            if used_a.contains(&idx_a) {
                continue;
            }
            let Some(key_a) = &keys_a[idx_a] else {
                continue;
            };
            if let Some(candidates) = key_map_b.get(key_a) {
                for &idx in candidates {
                    if cancelled() {
                        return false;
                    }
                    if !used_b.contains(&idx) && evidence.allows(idx_a, idx) {
                        let (similarity, details) =
                            DiffAlgorithms::compute_match_details_with_features(
                                func_a,
                                &functions_b[idx],
                                &features_a[idx_a],
                                &features_b[idx],
                            );
                        if similarity >= min_similarity {
                            let body_pairing_score = DiffAlgorithms::pairing_score(
                                func_a,
                                &functions_b[idx],
                                similarity,
                                &details,
                            );
                            let pairing_score =
                                evidence.pairing_score(idx_a, idx, body_pairing_score);
                            scored.push(Candidate {
                                a: idx_a,
                                b: idx,
                                similarity,
                                pairing_score,
                                ambiguity_margin: 0.0,
                                details,
                            });
                        }
                    }
                }
            }
        }
        for candidate in assign_candidates(scored) {
            if !used_a.contains(&candidate.a) && !used_b.contains(&candidate.b) {
                Self::push_match(
                    matches,
                    used_a,
                    used_b,
                    functions_a,
                    functions_b,
                    candidate.a,
                    candidate.b,
                    candidate.similarity,
                    candidate.ambiguity_margin,
                    candidate.details,
                    match_type.clone(),
                );
            }
        }
        true
    }

    /// Compare unmatched neighbors of binding seeds with a relaxed threshold.
    /// Resolve competing neighborhoods together; inferred pairs never seed
    /// another round of matching evidence.
    #[allow(clippy::too_many_arguments)]
    fn call_graph_propagation(
        &self,
        functions_a: &[FunctionInfo],
        functions_b: &[FunctionInfo],
        features_a: &[FunctionFeatures],
        features_b: &[FunctionFeatures],
        evidence: &TrustedMatchEvidence,
        matches: &mut Vec<FunctionMatch>,
        used_a: &mut FxHashSet<usize>,
        used_b: &mut FxHashSet<usize>,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> bool {
        let relaxed_threshold = (self.similarity_threshold - 0.25).max(0.35);

        let addr_to_idx_a: FxHashMap<u64, usize> = functions_a
            .iter()
            .enumerate()
            .map(|(i, f)| (f.address, i))
            .collect();
        let addr_to_idx_b: FxHashMap<u64, usize> = functions_b
            .iter()
            .enumerate()
            .map(|(i, f)| (f.address, i))
            .collect();

        let worklist: Vec<(usize, usize)> =
            evidence.anchors_a.iter().map(|(&a, &b)| (a, b)).collect();
        let mut scored_by_pair = FxHashMap::default();

        for (anchor_a, anchor_b) in worklist {
            if cancelled() {
                return false;
            }
            // Two neighborhoods: callees of the anchors, then callers.
            let neighborhoods = [
                (
                    &functions_a[anchor_a].callees,
                    &functions_b[anchor_b].callees,
                ),
                (
                    &functions_a[anchor_a].callers,
                    &functions_b[anchor_b].callers,
                ),
            ];

            for (neighbors_a, neighbors_b) in neighborhoods {
                let cand_a: Vec<usize> = neighbors_a
                    .iter()
                    .filter_map(|addr| addr_to_idx_a.get(addr).copied())
                    .filter(|i| !used_a.contains(i))
                    .collect();
                let cand_b: Vec<usize> = neighbors_b
                    .iter()
                    .filter_map(|addr| addr_to_idx_b.get(addr).copied())
                    .filter(|i| !used_b.contains(i))
                    .collect();

                if cand_a.is_empty() || cand_b.is_empty() {
                    continue;
                }

                // Score all pairs in this (small) neighborhood, then accept
                // greedily best-first for deterministic, conflict-free picks.
                for &ia in &cand_a {
                    for &ib in &cand_b {
                        if cancelled() {
                            return false;
                        }
                        if !evidence.allows(ia, ib) || scored_by_pair.contains_key(&(ia, ib)) {
                            continue;
                        }
                        let (similarity, details) =
                            DiffAlgorithms::compute_match_details_with_features(
                                &functions_a[ia],
                                &functions_b[ib],
                                &features_a[ia],
                                &features_b[ib],
                            );
                        if similarity >= relaxed_threshold {
                            let body_pairing_score = DiffAlgorithms::pairing_score(
                                &functions_a[ia],
                                &functions_b[ib],
                                similarity,
                                &details,
                            );
                            let pairing_score = evidence.pairing_score(ia, ib, body_pairing_score);
                            scored_by_pair.insert(
                                (ia, ib),
                                Candidate {
                                    a: ia,
                                    b: ib,
                                    similarity,
                                    pairing_score,
                                    ambiguity_margin: 0.0,
                                    details,
                                },
                            );
                        }
                    }
                }
            }
        }
        for candidate in assign_candidates(scored_by_pair.into_values().collect()) {
            if cancelled() {
                return false;
            }
            Self::push_match(
                matches,
                used_a,
                used_b,
                functions_a,
                functions_b,
                candidate.a,
                candidate.b,
                candidate.similarity,
                candidate.ambiguity_margin,
                candidate.details,
                MatchType::CallGraph,
            );
        }
        true
    }

    /// Fuzzy matching for remaining functions, blending primary and comprehensive similarity
    #[allow(clippy::too_many_arguments)]
    fn fuzzy_matching(
        &self,
        functions_a: &[FunctionInfo],
        functions_b: &[FunctionInfo],
        features_a: &[FunctionFeatures],
        features_b: &[FunctionFeatures],
        evidence: &TrustedMatchEvidence,
        matches: &mut Vec<FunctionMatch>,
        used_a: &mut FxHashSet<usize>,
        used_b: &mut FxHashSet<usize>,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Result<()> {
        if cancelled() {
            bail!("function matching cancelled");
        }
        // Tiny functions carry too little signal for fuzzy matching; they are
        // only matched by exact/name/call-graph evidence. For larger functions,
        // run expensive metrics only against the closest size/shape candidates.
        const MIN_FUZZY_INSTRUCTIONS: usize = 5;
        let sizes = |functions: &[FunctionInfo], used: &FxHashSet<usize>| {
            let mut result: Vec<_> = functions
                .iter()
                .enumerate()
                .filter(|(i, f)| {
                    !used.contains(i) && f.instructions.len() >= MIN_FUZZY_INSTRUCTIONS
                })
                .map(|(i, f)| (i, f.instructions.len()))
                .collect();
            result.sort_by_key(|&(i, count)| (count, functions[i].address));
            result
        };
        let a_by_size = sizes(functions_a, used_a);
        let b_by_size = sizes(functions_b, used_b);
        let rare_a = RareStringIndex::new(features_a, &a_by_size);
        let rare_b = RareStringIndex::new(features_b, &b_by_size);
        // Union both retrieval directions, so a crowded bucket on one side
        // cannot alone hide a strong counterpart retrieved from the other.
        let forward: Vec<Vec<_>> = a_by_size
            .par_iter()
            .map(|&(a, _)| {
                if cancelled() {
                    return Vec::new();
                }
                let mut candidates = shortlist(
                    &functions_a[a],
                    &features_a[a],
                    functions_b,
                    features_b,
                    &b_by_size,
                );
                candidates.extend(rare_b.shortlist(
                    &functions_a[a],
                    &features_a[a],
                    functions_b,
                    features_b,
                ));
                candidates.into_iter().map(|b| (a, b)).collect()
            })
            .collect();
        let reverse: Vec<Vec<_>> = b_by_size
            .par_iter()
            .map(|&(b, _)| {
                if cancelled() {
                    return Vec::new();
                }
                let mut candidates = shortlist(
                    &functions_b[b],
                    &features_b[b],
                    functions_a,
                    features_a,
                    &a_by_size,
                );
                candidates.extend(rare_a.shortlist(
                    &functions_b[b],
                    &features_b[b],
                    functions_a,
                    features_a,
                ));
                candidates.into_iter().map(|a| (a, b)).collect()
            })
            .collect();
        let pairs: FxHashSet<_> = forward.into_iter().chain(reverse).flatten().collect();
        let candidates: Vec<_> = pairs.into_iter().collect();
        let candidates: Vec<_> = candidates
            .par_iter()
            .filter_map(|&(idx_a, idx_b)| {
                if cancelled() || !evidence.allows(idx_a, idx_b) {
                    return None;
                }
                let func_a = &functions_a[idx_a];
                let func_b = &functions_b[idx_b];
                let (primary, details) = DiffAlgorithms::compute_match_details_with_features(
                    func_a,
                    func_b,
                    &features_a[idx_a],
                    &features_b[idx_b],
                );
                // comprehensive is at most 1.0. Skip its quadratic sequence metric
                // when no possible result could pass the configured gates.
                let upper = primary * 0.6 + 0.4;
                if upper < self.similarity_threshold
                    || DiffAlgorithms::confidence_for_match(&MatchType::Heuristic, upper)
                        < self.confidence_threshold
                {
                    return None;
                }
                let comprehensive = DiffAlgorithms::comprehensive_similarity_with_features(
                    func_a,
                    func_b,
                    &features_a[idx_a],
                    &features_b[idx_b],
                );
                let similarity = (primary * 0.6 + comprehensive * 0.4).clamp(0.0, 1.0);
                if similarity < self.similarity_threshold
                    || DiffAlgorithms::confidence_for_match(&MatchType::Heuristic, similarity)
                        < self.confidence_threshold
                {
                    return None;
                }
                let body_score =
                    DiffAlgorithms::pairing_score(func_a, func_b, similarity, &details);
                Some(Candidate {
                    a: idx_a,
                    b: idx_b,
                    similarity,
                    pairing_score: evidence.pairing_score(idx_a, idx_b, body_score),
                    ambiguity_margin: 0.0,
                    details,
                })
            })
            .collect();
        if cancelled() {
            bail!("function matching cancelled");
        }

        for candidate in assign_candidates(candidates) {
            if !used_a.contains(&candidate.a) && !used_b.contains(&candidate.b) {
                Self::push_match(
                    matches,
                    used_a,
                    used_b,
                    functions_a,
                    functions_b,
                    candidate.a,
                    candidate.b,
                    candidate.similarity,
                    candidate.ambiguity_margin,
                    candidate.details,
                    MatchType::Heuristic,
                );
            }
        }

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{BasicBlockInfo, InstructionInfo};

    fn spp_function(name: &str, address: u64, immediate: &str) -> FunctionInfo {
        let instructions: Vec<InstructionInfo> = ["mov", "add", "xor", "cmp", "ret"]
            .into_iter()
            .enumerate()
            .map(|(index, mnemonic)| InstructionInfo {
                address: address + index as u64,
                mnemonic: mnemonic.into(),
                operands: if index == 0 {
                    vec![immediate.into()]
                } else {
                    Vec::new()
                },
                length: 1,
                ..Default::default()
            })
            .collect();
        FunctionInfo {
            name: name.into(),
            address,
            size: instructions.len() as u64,
            basic_blocks: vec![BasicBlockInfo {
                address,
                size: instructions.len() as u64,
                instructions: instructions.clone(),
                instruction_count: instructions.len(),
                ..Default::default()
            }],
            instructions,
            instruction_count: 5,
            ..Default::default()
        }
    }

    #[test]
    fn generated_name_policy_preserves_analyst_names() {
        assert!(is_auto_generated_name("sub_1000"));
        assert!(is_auto_generated_name("FUN_ABCDEF"));
        assert!(is_auto_generated_name("nullsub_3"));
        assert!(is_auto_generated_name("fcn.00401000"));
        assert!(is_auto_generated_name("j__memcpy"));
        assert!(!is_auto_generated_name("sub_parse_header"));
        assert!(!is_auto_generated_name("parse_header"));
    }

    #[test]
    fn keyed_matching_assigns_best_pairs_globally() {
        let function = |name: &str, address: u64| FunctionInfo {
            name: name.into(),
            address,
            ..Default::default()
        };
        let a = vec![function("fo", 0x1000), function("foo", 0x2000)];
        let b = vec![function("foo", 0x3000), function("fox", 0x4000)];
        let keys_a = vec![Some(1u8), Some(1u8)];
        let keys_b = vec![Some(1u8), Some(1u8)];
        let features_a: Vec<_> = a.iter().map(DiffAlgorithms::function_features).collect();
        let features_b: Vec<_> = b.iter().map(DiffAlgorithms::function_features).collect();
        let evidence = TrustedMatchEvidence::new(&a, &b);
        let mut matches = Vec::new();
        let mut used_a = FxHashSet::default();
        let mut used_b = FxHashSet::default();

        assert!(MatchingEngine::new().keyed_matching(
            &a,
            &b,
            &features_a,
            &features_b,
            &keys_a,
            &keys_b,
            MatchType::Structural,
            0.0,
            &evidence,
            &mut matches,
            &mut used_a,
            &mut used_b,
            &|| false,
        ));
        assert!(matches
            .iter()
            .any(|m| { m.function_a.address == 0x2000 && m.function_b.address == 0x3000 }));
        assert!(matches
            .iter()
            .any(|m| { m.function_a.address == 0x1000 && m.function_b.address == 0x4000 }));
    }

    #[test]
    fn ambiguity_component_uses_maximum_weight_assignment() {
        let candidate = |a, b, score| Candidate {
            a,
            b,
            similarity: score,
            pairing_score: score,
            ambiguity_margin: 0.0,
            details: MatchDetails::default(),
        };
        let selected = assign_candidates(vec![
            candidate(0, 0, 0.90),
            candidate(0, 1, 0.80),
            candidate(1, 0, 0.80),
            candidate(1, 1, 0.10),
        ]);
        let pairs: Vec<_> = selected
            .iter()
            .map(|candidate| (candidate.a, candidate.b))
            .collect();
        assert_eq!(pairs, vec![(0, 1), (1, 0)]);

        let reversed = assign_candidates(
            selected
                .into_iter()
                .map(|candidate| Candidate {
                    a: candidate.b,
                    b: candidate.a,
                    ..candidate
                })
                .collect(),
        );
        let reversed_pairs: Vec<_> = reversed
            .iter()
            .map(|candidate| (candidate.a, candidate.b))
            .collect();
        assert_eq!(reversed_pairs, vec![(0, 1), (1, 0)]);
    }

    #[test]
    fn ambiguity_component_prioritizes_cardinality_over_one_strong_edge() {
        let candidate = |a, b, score| Candidate {
            a,
            b,
            similarity: score,
            pairing_score: score,
            ambiguity_margin: 0.0,
            details: MatchDetails::default(),
        };
        let selected = assign_candidates(vec![
            candidate(0, 0, 1.0),
            candidate(0, 1, 0.0),
            candidate(1, 0, 0.0),
        ]);
        let pairs: Vec<_> = selected
            .iter()
            .map(|candidate| (candidate.a, candidate.b))
            .collect();
        assert_eq!(pairs, vec![(0, 1), (1, 0)]);
    }

    type LatticeChain = (
        Vec<FunctionInfo>,
        Vec<FunctionInfo>,
        Vec<Option<u8>>,
        Vec<Option<u8>>,
    );

    fn lattice_chain() -> LatticeChain {
        let function = |address, callers: Vec<u64>, callees: Vec<u64>| FunctionInfo {
            address,
            callers,
            callees,
            ..Default::default()
        };
        let a = vec![
            function(0x1000, vec![], vec![0x1100]),
            function(0x1100, vec![0x1000], vec![0x1200]),
            function(0x1200, vec![0x1100], vec![]),
        ];
        let b = vec![
            function(0x2000, vec![], vec![0x2100]),
            function(0x2100, vec![0x2000], vec![0x2200]),
            function(0x2200, vec![0x2100], vec![]),
        ];
        (
            a,
            b,
            vec![Some(0), Some(1), Some(1)],
            vec![Some(0), Some(1), Some(1)],
        )
    }

    #[test]
    fn context_pruning_removes_contradictions_and_unsupported_dependents() {
        let (mut a, b, ka, kb) = lattice_chain();
        // The second tentative pair contradicts a new unmatched call edge.
        let anchor_address = a[0].address;
        a[1].callees.push(anchor_address);
        let child_address = a[2].address;
        a[1].callers.push(child_address);
        let parent_address = a[1].address;
        a[2].callees.push(parent_address);
        a[0].callers.push(parent_address);
        let mut e = TrustedMatchEvidence::new(&a, &b);
        e.seed_unique(&ka, &kb, &|| false).unwrap();
        e.context_a.extend([(1, 1), (2, 2)]);
        e.context_b.extend([(1, 1), (2, 2)]);
        assert_eq!(e.prune_context(&|| false).unwrap().len(), 2);
        assert!(e.context_a.is_empty());
        assert!(e.context_b.is_empty());
        assert_eq!(e.anchors_a.len(), 1);
    }

    #[test]
    fn tentative_context_discriminates_neighbors_without_binding_endpoints() {
        let (a, b, ka, kb) = lattice_chain();
        let mut evidence = TrustedMatchEvidence::new(&a, &b);
        evidence.seed_unique(&ka, &kb, &|| false).unwrap();
        assert_eq!(evidence.neighbor_counts(2, 2), (0, 0));
        evidence.context_a.insert(1, 1);
        evidence.context_b.insert(1, 1);
        assert_eq!(evidence.neighbor_counts(2, 2), (1, 0));
        assert!(evidence.allows(1, 2));
        assert!(evidence.allows(2, 1));
        assert_eq!(evidence.anchors_a.len(), 1);
        assert!(evidence.pairing_score(2, 2, 1.0) > evidence.pairing_score(2, 1, 1.0));
    }

    #[test]
    fn indexed_neighbor_candidates_equal_exhaustive_positive_support() {
        let functions = |base: u64, seed: usize| {
            (0..12)
                .map(|i| FunctionInfo {
                    address: base + i as u64,
                    // Intentionally nonreciprocal caller and callee lists.
                    callers: (0..12)
                        .filter(|&j| (i * 17 + j * 11 + seed) % 7 < 2)
                        .map(|j| base + j as u64)
                        .collect(),
                    callees: (0..12)
                        .filter(|&j| (i * 13 + j * 19 + seed) % 9 < 2)
                        .map(|j| base + j as u64)
                        .collect(),
                    ..Default::default()
                })
                .collect::<Vec<_>>()
        };
        for seed in 0..16 {
            let a = functions(0x1000, seed);
            let b = functions(0x2000, seed + 3);
            let mut evidence = TrustedMatchEvidence::new(&a, &b);
            evidence.anchors_a.extend([(0, 0), (1, 1)]);
            evidence.anchors_b.extend([(0, 0), (1, 1)]);
            evidence.context_a.extend([(2, 3), (3, 2)]);
            evidence.context_b.extend([(3, 2), (2, 3)]);
            for (aa, bb) in [
                ((0..12).collect::<Vec<_>>(), (0..12).collect::<Vec<_>>()),
                (vec![4, 6, 8, 10], vec![5, 7, 9, 11]),
            ] {
                let actual: FxHashSet<_> = evidence
                    .neighbor_supported_pairs(&aa, &bb, &|| false)
                    .unwrap()
                    .into_iter()
                    .collect();
                let expected: FxHashSet<_> = aa
                    .iter()
                    .flat_map(|&a| bb.iter().map(move |&b| (a, b)))
                    .filter(|&(a, b)| evidence.neighbor_counts(a, b).0 > 0)
                    .collect();
                assert_eq!(actual, expected, "seed {seed}");
            }
        }
    }

    #[test]
    fn duplicate_buckets_remain_uncertain_and_seeds_are_binding() {
        let (a, b, ka, kb) = lattice_chain();
        let mut evidence = TrustedMatchEvidence::new(&a, &b);
        evidence.seed_unique(&ka, &kb, &|| false).unwrap();
        evidence.seed_unique(&ka, &kb, &|| false).unwrap();
        assert_eq!(evidence.anchors_a.len(), 1);
        assert!(evidence.allows(0, 0));
        assert!(!evidence.allows(0, 1));
        assert!(!evidence.allows(1, 0));
        assert!(evidence.allows(1, 2));
        assert!(evidence.pairing_score(1, 1, 1.0) > evidence.pairing_score(1, 2, 1.0));
        // A speculative match at node 1 cannot recursively validate node 2.
        assert_eq!(evidence.neighbor_counts(2, 2), (0, 0));
    }

    #[test]
    fn complete_seed_set_includes_contradictions_even_for_anchors() {
        let (mut a, b, _, _) = lattice_chain();
        a[1].callees.clear();
        a[2].callers.clear();
        let keys = vec![Some(0), Some(1), Some(2)];
        let mut evidence = TrustedMatchEvidence::new(&a, &b);
        evidence.seed_unique(&keys, &keys, &|| false).unwrap();
        assert_eq!(evidence.neighbor_counts(1, 1), (1, 1));
        let mut reversed = TrustedMatchEvidence::new(&b, &a);
        reversed.seed_unique(&keys, &keys, &|| false).unwrap();
        assert_eq!(reversed.neighbor_counts(1, 1), (1, 1));
    }

    #[test]
    fn cancellation_precedes_feature_extraction() {
        let engine = MatchingEngine::new();
        assert!(engine
            .match_functions_cancellable(&[FunctionInfo::default()], &[], &|| true)
            .is_err());
    }

    #[test]
    fn keyed_assignment_cannot_override_binding_anchors() {
        let a = vec![
            spp_function("alpha", 0x1000, "1"),
            spp_function("beta", 0x1100, "2"),
        ];
        let b = vec![
            spp_function("alpha", 0x2000, "2"),
            spp_function("beta", 0x2100, "1"),
        ];
        let names = vec![Some("alpha"), Some("beta")];
        let keys = vec![Some(1u8), Some(1u8)];
        let mut evidence = TrustedMatchEvidence::new(&a, &b);
        evidence.seed_unique(&names, &names, &|| false).unwrap();
        let fa: Vec<_> = a.iter().map(DiffAlgorithms::function_features).collect();
        let fb: Vec<_> = b.iter().map(DiffAlgorithms::function_features).collect();
        let mut matches = vec![];
        assert!(MatchingEngine::new().keyed_matching(
            &a,
            &b,
            &fa,
            &fb,
            &keys,
            &keys,
            MatchType::MdIndex,
            0.0,
            &evidence,
            &mut matches,
            &mut FxHashSet::default(),
            &mut FxHashSet::default(),
            &|| false
        ));
        assert_eq!(matches.len(), 2);
        assert!(matches
            .iter()
            .all(|m| m.function_a.name == m.function_b.name));
    }

    #[test]
    fn fuzzy_retrieval_scores_late_equal_size_candidates_and_survives_permutation() {
        let target = spp_function("sub_1000", 0x1000, "777");
        let mut others: Vec<_> = (0..600)
            .map(|i| spp_function(&format!("sub_{:x}", 0x2000 + i), 0x2000 + i, "1"))
            .collect();
        others.push(spp_function("sub_9000", 0x9000, "777"));
        let feature = DiffAlgorithms::function_features(&target);
        let retrieve = |functions: &[FunctionInfo]| {
            let features: Vec<_> = functions
                .iter()
                .map(DiffAlgorithms::function_features)
                .collect();
            let mut sizes: Vec<_> = functions
                .iter()
                .enumerate()
                .map(|(i, f)| (i, f.instructions.len()))
                .collect();
            sizes.sort_by_key(|&(i, size)| (size, functions[i].address));
            shortlist(&target, &feature, functions, &features, &sizes)
                .into_iter()
                .map(|i| functions[i].address)
                .collect::<Vec<_>>()
        };
        let forward = retrieve(&others);
        assert_eq!(forward[0], 0x9000);
        others.reverse();
        assert_eq!(forward, retrieve(&others));
    }

    #[test]
    fn fuzzy_retrieval_recovers_rare_string_pair_outside_size_window() {
        let function = |address: u64, count: usize, mnemonic: &str, marker: bool| {
            let instructions: Vec<_> = (0..count)
                .map(|i| InstructionInfo {
                    address: address + i as u64,
                    mnemonic: mnemonic.into(),
                    operands: vec![i.to_string()],
                    length: 1,
                    ..Default::default()
                })
                .collect();
            FunctionInfo {
                name: format!("sub_{address:x}"),
                address,
                size: count as u64,
                basic_blocks: vec![BasicBlockInfo {
                    address,
                    size: count as u64,
                    instructions: instructions.clone(),
                    instruction_count: count,
                    mnemonic_hash: mnemonic.into(),
                    ..Default::default()
                }],
                instructions,
                instruction_count: count,
                strings: if marker {
                    vec!["unique protocol marker".into()]
                } else {
                    Vec::new()
                },
                ..Default::default()
            }
        };
        let target_a = function(0x1000, 50, "mov", true);
        let mut target_b = function(0x900000, 65, "mov", true);
        target_b.instructions[50..]
            .iter_mut()
            .for_each(|instruction| {
                instruction.mnemonic = "nop".into();
                instruction.operands.clear();
            });
        target_b.basic_blocks[0].instructions = target_b.instructions.clone();
        let mut a = vec![target_a];
        let mut b = vec![target_b];
        for i in 0..257 {
            let count = 51 + i % 14;
            a.push(function(0x10000 + i as u64 * 0x100, count, "xor", false));
            b.push(function(0x30000 + i as u64 * 0x100, count, "sub", false));
        }
        let matches = MatchingEngine::new().match_functions(&a, &b).unwrap();
        assert!(matches
            .iter()
            .any(|m| { m.function_a.address == 0x1000 && m.function_b.address == 0x900000 }));
        let reversed = MatchingEngine::new().match_functions(&b, &a).unwrap();
        assert!(reversed
            .iter()
            .any(|m| { m.function_a.address == 0x900000 && m.function_b.address == 0x1000 }));

        // The string only nominates a candidate; unrelated bodies still fail
        // the existing similarity gate.
        let mut unrelated = b.clone();
        for instruction in &mut unrelated[0].instructions {
            instruction.mnemonic = "sub".into();
        }
        unrelated[0].basic_blocks[0].instructions = unrelated[0].instructions.clone();
        unrelated[0].basic_blocks[0].mnemonic_hash = "sub".into();
        let rejected = MatchingEngine::new()
            .match_functions(&a, &unrelated)
            .unwrap();
        assert!(!rejected
            .iter()
            .any(|m| { m.function_a.address == 0x1000 && m.function_b.address == 0x900000 }));
    }

    #[test]
    fn duplicate_exact_bodies_are_not_reported_as_certain_identity() {
        let a = vec![
            spp_function("sub_1000", 0x1000, "1"),
            spp_function("sub_1100", 0x1100, "1"),
        ];
        let b = vec![
            spp_function("sub_2000", 0x2000, "1"),
            spp_function("sub_2100", 0x2100, "1"),
        ];
        let matches = MatchingEngine::new().match_functions(&a, &b).unwrap();
        assert_eq!(matches.len(), 2);
        assert!(matches
            .iter()
            .all(|m| m.confidence <= 0.5 && !m.details.binding_anchor));
    }

    #[test]
    fn duplicate_assignment_is_independent_of_extraction_order() {
        let a = vec![
            spp_function("sub_1000", 0x1000, "1"),
            spp_function("sub_1100", 0x1100, "1"),
        ];
        let mut b = vec![
            spp_function("sub_2000", 0x2000, "1"),
            spp_function("sub_2100", 0x2100, "1"),
        ];
        let pairs = |b: &[FunctionInfo]| {
            MatchingEngine::new()
                .match_functions(&a, b)
                .unwrap()
                .iter()
                .map(|m| (m.function_a.address, m.function_b.address))
                .collect::<Vec<_>>()
        };
        let expected = pairs(&b);
        b.reverse();
        assert_eq!(expected, pairs(&b));
    }

    #[test]
    fn unique_names_are_reserved_before_duplicate_body_assignment() {
        let a = vec![
            spp_function("alpha", 0x1000, "1"),
            spp_function("beta", 0x1100, "1"),
        ];
        let b = vec![
            spp_function("beta", 0x2000, "1"),
            spp_function("alpha", 0x2100, "1"),
        ];
        let matches = MatchingEngine::new().match_functions(&a, &b).unwrap();
        assert_eq!(matches.len(), 2);
        assert!(matches
            .iter()
            .all(|m| m.function_a.name == m.function_b.name && m.details.binding_anchor));
    }

    #[test]
    fn assignment_reports_local_ambiguity_margin() {
        let candidate = |b, score| Candidate {
            a: 0,
            b,
            similarity: score,
            pairing_score: score,
            ambiguity_margin: 0.0,
            details: MatchDetails::default(),
        };
        let selected = assign_candidates(vec![candidate(0, 0.9), candidate(1, 0.8)]);
        assert_eq!(selected.len(), 1);
        assert_eq!(selected[0].b, 0);
        assert!((selected[0].ambiguity_margin - 0.1).abs() < 1e-9);
    }

    #[test]
    fn small_primes_operand_change_is_not_identical() {
        let mut a = vec![spp_function("sub_1000", 0x1000, "1")];
        let mut b = vec![spp_function("sub_2000", 0x2000, "2")];
        crate::BinaryDiffEngine::preprocess_functions(&mut a);
        crate::BinaryDiffEngine::preprocess_functions(&mut b);

        let matches = MatchingEngine::new().match_functions(&a, &b).unwrap();
        assert_eq!(matches.len(), 1);
        assert_eq!(matches[0].match_type, MatchType::SmallPrimes);
        assert!(matches[0].similarity < 0.999);
        assert!(matches[0].confidence < 0.90);
    }
}
