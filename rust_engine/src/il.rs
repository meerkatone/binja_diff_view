//! IL-aware (semantic) diffing.
//!
//! A plain textual diff of rendered IL is misleading: a *renamed* callee and a
//! *replaced* callee produce identical text diffs even though one is cosmetic
//! and the other is a real semantic change. The fix is to diff a *normalized*
//! token stream — generated variable names and relocated addresses are
//! canonicalized, while semantic values such as constants are preserved, and
//! matched-callee renames are resolved
//! through a rename map — so the differ compares structure, not surface text.
//!
//! The frontend (Binary Ninja Python) extracts a typed token stream per IL
//! line via `InstructionTextToken`; this module is pure and testable without
//! Binary Ninja.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;

pub(crate) type VariableMap = HashMap<String, String>;

/// One typed token of a rendered IL line. `kind` mirrors Binary Ninja's
/// `InstructionTextToken` type name (e.g. "register", "localVariable",
/// "integer", "codeSymbol"); unknown kinds are treated as literal text.
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct IlToken {
    pub kind: String,
    pub text: String,
}

/// One rendered IL line as a typed token stream, plus the original rendered
/// text (kept verbatim for display).
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct IlLine {
    pub tokens: Vec<IlToken>,
    pub text: String,
}

/// A function's IL at one level (LLIL/MLIL/HLIL/Pseudo-C).
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct IlFunction {
    pub level: String,
    pub lines: Vec<IlLine>,
}

/// Request payload for the IL-diff FFI: two IL functions plus an optional
/// callee rename map (symbol-in-A -> symbol-in-B for callees that the matcher
/// paired). Renamed-but-matched calls normalize equal; everything else that
/// differs is a genuine change.
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct IlDiffRequest {
    pub a: IlFunction,
    pub b: IlFunction,
    /// symbol-in-A -> symbol-in-B for matched callees.
    pub rename_map: HashMap<String, String>,
    /// code-address-in-A -> code-address-in-B for matched callees (rendered as
    /// literal addresses). Lets a relocated-but-matched call target normalize
    /// equal instead of reading as a false change. Keys/values are the rendered
    /// address text (e.g. "0x1800010e8").
    pub addr_map: HashMap<String, String>,
}

/// Per-line diff op.
/// - `equal`: identical original text.
/// - `rename`: canonical form matches but original text differs (cosmetic:
///   variable renumbering, a matched-callee rename) — *not* a semantic change.
/// - `replace`: canonical forms differ; a real change. Paired A/B lines.
/// - `insert` / `delete`: line only on one side.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum DiffOp {
    Equal,
    Rename,
    Replace,
    Insert,
    Delete,
}

/// A token span for inline highlighting: the rendered text plus whether it
/// changed relative to the paired line (token-level LCS over canonical forms).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TokenSpan {
    pub text: String,
    pub changed: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiffLine {
    pub op: DiffOp,
    pub a: Option<String>,
    pub b: Option<String>,
    /// Per-token spans for the A/B side of a `replace`/`rename` line, so the UI
    /// can highlight just the changed tokens. Empty for whole-line ops.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub a_spans: Vec<TokenSpan>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub b_spans: Vec<TokenSpan>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IlDiff {
    pub lines: Vec<DiffLine>,
    /// Fraction of canonical lines that are unchanged (LCS / max len).
    pub similarity: f64,
}

/// Canonicalize one token. Generated locals and relocated data addresses are
/// normalized, while constants and analyst-provided identities remain literal.
/// Callee symbols are resolved through `rename_map` (forward, A->B) so a matched
/// rename normalizes equal on both sides.
fn is_hex(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// Normalize only variable names Binary Ninja is known to synthesize. Analyst
/// names remain literal: collapsing every variable to one `VAR` token hides
/// operand swaps such as `dst = src` -> `src = dst`.
fn generated_variable_base(text: &str) -> Option<String> {
    let text = text.trim();
    if let Some(storage) = text.strip_prefix("var_") {
        if is_hex(storage) {
            return Some(text.to_string());
        }
    }
    let base = strip_generated_dup_suffix(text);
    if let Some(storage) = base.strip_prefix("var_") {
        if is_hex(storage) {
            return Some(base.to_string());
        }
    }
    None
}

fn is_variable_token(tok: &IlToken) -> bool {
    let kind = tok.kind.to_ascii_lowercase();
    kind.contains("variable") || kind.contains("localvar") || kind.contains("stackvar")
}

pub(crate) fn variable_map_for_lines<'a>(
    lines: impl IntoIterator<Item = &'a IlLine>,
) -> VariableMap {
    let mut variables = VariableMap::new();
    for line in lines {
        for token in &line.tokens {
            if !is_variable_token(token) {
                continue;
            }
            let Some(base) = generated_variable_base(&token.text) else {
                continue;
            };
            let next = variables.len();
            variables
                .entry(base)
                .or_insert_with(|| format!("AUTO_LOCAL:{next}"));
        }
    }
    variables
}

fn canonical_variable(text: &str, variable_map: &VariableMap) -> String {
    let text = text.trim();
    if let Some(generated) = generated_variable_base(text) {
        return variable_map
            .get(&generated)
            .cloned()
            .unwrap_or_else(|| format!("AUTO_LOCAL:{generated}"));
    }
    // Parse the argument index before considering a duplicate suffix: the
    // final number in arg_1 is its identity, not a disambiguation suffix.
    for base in [text, strip_generated_dup_suffix(text)] {
        if let Some(index) = base
            .strip_prefix("arg_")
            .or_else(|| base.strip_prefix("arg"))
        {
            if !index.is_empty() && index.bytes().all(|b| b.is_ascii_digit()) {
                return format!("ARG:{index}");
            }
        }
    }
    // Duplicate suffixes are cosmetic only for recognized generated names.
    // Analyst/debug names such as buffer_128 and buffer_256 are distinct.
    format!("VAR:{text}")
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TokenRole {
    Normal,
    BranchLabel,
    BranchAddress,
}

fn is_branch_label_kind(kind: &str) -> bool {
    kind.contains("gotolabel") || kind.contains("integer") || kind.contains("possiblevalue")
}

fn ends_with_control_target_marker(text: &str) -> bool {
    let text = text.trim().to_ascii_lowercase();
    text.ends_with("goto")
        || text.ends_with("then")
        || text.ends_with("else")
        || text.ends_with("=>")
}

/// Identify graph-local labels and their rendered addresses from token
/// context. Some Binary Ninja IL renderers emit the numeric label as an
/// IntegerToken or PossibleValueToken instead of GotoLabelToken, for example
/// `goto 112 @ 0x1000493bc`. The `@`/control-keyword context distinguishes the
/// label from real constants in predicates such as `length >= 0x100`.
fn token_roles(line: &IlLine) -> Vec<TokenRole> {
    let mut roles = vec![TokenRole::Normal; line.tokens.len()];
    for (index, token) in line.tokens.iter().enumerate() {
        let kind = token.kind.to_ascii_lowercase();
        if kind.contains("gotolabel") {
            roles[index] = TokenRole::BranchLabel;
            continue;
        }
        if !is_branch_label_kind(&kind) {
            continue;
        }

        let preceded_by_control = line.tokens[..index]
            .iter()
            .rev()
            .find(|candidate| !candidate.text.trim().is_empty())
            .is_some_and(|candidate| ends_with_control_target_marker(&candidate.text));
        let followed_by_at = line.tokens[index + 1..]
            .iter()
            .find(|candidate| !candidate.text.trim().is_empty())
            .is_some_and(|candidate| candidate.text.contains('@'));
        if preceded_by_control || followed_by_at {
            roles[index] = TokenRole::BranchLabel;
        }
    }

    for (index, token) in line.tokens.iter().enumerate() {
        let kind = token.kind.to_ascii_lowercase();
        if !(kind.contains("coderelative") || kind.contains("codeaddress")) {
            continue;
        }
        let rendered_after_at = line.tokens[..index]
            .iter()
            .rev()
            .find(|candidate| !candidate.text.trim().is_empty())
            .is_some_and(|candidate| candidate.text.contains('@'));
        if rendered_after_at {
            roles[index] = TokenRole::BranchAddress;
        }
    }
    roles
}

fn generated_data_symbol_class(text: &str) -> Option<&'static str> {
    let normalized = text.trim().trim_start_matches('_').to_ascii_lowercase();
    let prefixes: &[(&str, &str)] = &[
        ("jump_table_", "GENERATED_JUMP_TABLE"),
        ("data_", "GENERATED_DATA"),
        ("byte_", "GENERATED_DATA"),
        ("word_", "GENERATED_DATA"),
        ("dword_", "GENERATED_DATA"),
        ("qword_", "GENERATED_DATA"),
        ("oword_", "GENERATED_DATA"),
        ("off_", "GENERATED_DATA"),
        ("unk_", "GENERATED_DATA"),
    ];
    prefixes.iter().find_map(|(prefix, class)| {
        normalized
            .strip_prefix(prefix)
            .filter(|suffix| is_hex(suffix))
            .map(|_| *class)
    })
}

fn canonical_token(
    tok: &IlToken,
    rename_map: &HashMap<String, String>,
    addr_map: &HashMap<String, String>,
    variable_map: &VariableMap,
    role: TokenRole,
) -> String {
    // Binary Ninja InstructionTextToken type names are matched case-insensitively
    // and loosely (contains) so LLIL/MLIL/HLIL variants all collapse correctly.
    let kind = tok.kind.to_ascii_lowercase();
    if is_variable_token(tok) {
        return canonical_variable(&tok.text, variable_map);
    }
    // Binary Ninja numbers goto labels within each rendered graph. Inserting
    // one block can renumber every later target without changing the target
    // relationship represented by the CFG.
    if role == TokenRole::BranchLabel || kind.contains("gotolabel") {
        return "GOTO_LABEL".to_string();
    }
    // Constants are semantic. In particular, bounds and bit masks are common
    // patch-diff signals and must not be normalized away.
    if kind.contains("integer") || kind.contains("possiblevalue") || kind.contains("floatingpoint")
    {
        return format!("CONST:{}", tok.text.trim().to_ascii_lowercase());
    }
    // Code/call-target addresses are kept literal so a genuinely changed call
    // target surfaces as a real change. A matched callee that merely relocated
    // is resolved through addr_map to its B-side address so it normalizes equal.
    // Data/global addresses relocate unpredictably and are collapsed to ADDR.
    if kind.contains("coderelative") || kind.contains("codeaddress") {
        if role == TokenRole::BranchAddress {
            return "GOTO_ADDRESS".to_string();
        }
        let text = tok.text.trim();
        let resolved = addr_map.get(text).map(String::as_str).unwrap_or(text);
        return resolved.to_string();
    }
    if kind.contains("address") {
        return "ADDR".to_string();
    }
    if kind.contains("symbol") || kind.contains("import") || kind.contains("indirect") {
        // Callers apply this A-to-B map only while canonicalizing side A.
        // Function rename evidence must not rewrite a same-named data symbol.
        let resolved = if kind.contains("data") {
            tok.text.clone()
        } else {
            rename_map
                .get(&tok.text)
                .cloned()
                .unwrap_or_else(|| tok.text.clone())
        };
        if kind.contains("data") {
            if let Some(class) = generated_data_symbol_class(&resolved) {
                return format!("SYM:{class}");
            }
        }
        // Binary Ninja sometimes appends a short duplicate suffix to generated
        // data symbols. Do not do this for arbitrary symbols: AES_128 and
        // AES_256, for example, are meaningfully different.
        let normalized =
            if kind.contains("data") && (resolved.starts_with("__") || resolved.contains('@')) {
                strip_generated_dup_suffix(&resolved)
            } else {
                &resolved
            };
        return format!("SYM:{normalized}");
    }
    // Registers, keywords, operators, punctuation, raw text: kept literally
    // (they carry structural meaning). Whitespace-only tokens are dropped.
    let t = tok.text.trim();
    if t.is_empty() {
        String::new()
    } else {
        t.to_string()
    }
}

/// Strip a trailing `_<n>` (1-3 digits) that Binary Ninja appends to
/// disambiguate generated variables/data symbols across builds (e.g.
/// `__xmm@..0303_1` -> `__xmm@..0303`). Longer numeric suffixes are left
/// intact so real identifiers (e.g. `Feature_1196105017`) are not mangled.
fn strip_generated_dup_suffix(s: &str) -> &str {
    if let Some(pos) = s.rfind('_') {
        let suffix = &s[pos + 1..];
        if !suffix.is_empty()
            && suffix.len() <= 3
            && suffix.bytes().all(|b| b.is_ascii_digit())
            && pos > 0
        // keep at least one leading char
        {
            return &s[..pos];
        }
    }
    s
}

/// Canonical key for a whole line: space-joined non-empty canonical tokens.
/// Falls back to the trimmed rendered text when no typed tokens are present.
pub(crate) fn canonical_line(
    line: &IlLine,
    rename_map: &HashMap<String, String>,
    addr_map: &HashMap<String, String>,
    variable_map: &VariableMap,
) -> String {
    if line.tokens.is_empty() {
        return line.text.split_whitespace().collect::<Vec<_>>().join(" ");
    }
    let roles = token_roles(line);
    line.tokens
        .iter()
        .zip(roles)
        .map(|(t, role)| canonical_token(t, rename_map, addr_map, variable_map, role))
        .filter(|s| !s.is_empty())
        .collect::<Vec<_>>()
        .join(" ")
}

/// An aligned line op referencing original A/B line indices (built during
/// backtrack, before paired lines get their token-level spans).
enum Aligned {
    Equal(usize, usize),
    Rename(usize, usize),
    Replace(usize, usize),
    Delete(usize),
    Insert(usize),
}

/// Exact Hirschberg LCS matches in O(a*b) time and O(b) auxiliary memory.
/// Choosing the smallest split on ties mirrors the full matrix backtracker's
/// preference for advancing A when the two remaining LCS lengths are equal.
fn linear_memory_lcs_matches<T: Eq>(a: &[T], b: &[T]) -> Vec<(usize, usize)> {
    fn prefix_lengths<T: Eq>(a: &[T], b: &[T]) -> Vec<usize> {
        let mut previous = vec![0usize; b.len() + 1];
        let mut current = vec![0usize; b.len() + 1];
        for left in a {
            for (j, right) in b.iter().enumerate() {
                current[j + 1] = if left == right {
                    previous[j] + 1
                } else {
                    previous[j + 1].max(current[j])
                };
            }
            std::mem::swap(&mut previous, &mut current);
            current.fill(0);
        }
        previous
    }

    fn suffix_lengths<T: Eq>(a: &[T], b: &[T]) -> Vec<usize> {
        let mut previous = vec![0usize; b.len() + 1];
        let mut current = vec![0usize; b.len() + 1];
        for left in a.iter().rev() {
            for j in (0..b.len()).rev() {
                current[j] = if left == &b[j] {
                    previous[j + 1] + 1
                } else {
                    previous[j].max(current[j + 1])
                };
            }
            std::mem::swap(&mut previous, &mut current);
            current.fill(0);
        }
        previous
    }

    fn recurse<T: Eq>(
        a: &[T],
        b: &[T],
        offset_a: usize,
        offset_b: usize,
        out: &mut Vec<(usize, usize)>,
    ) {
        if a.is_empty() || b.is_empty() {
            return;
        }
        if a.len() == 1 {
            if let Some(j) = b.iter().position(|right| &a[0] == right) {
                out.push((offset_a, offset_b + j));
            }
            return;
        }

        let middle = a.len() / 2;
        let left = prefix_lengths(&a[..middle], b);
        let right = suffix_lengths(&a[middle..], b);
        let split = (0..=b.len())
            .max_by_key(|&j| (left[j] + right[j], std::cmp::Reverse(j)))
            .unwrap_or(0);
        recurse(&a[..middle], &b[..split], offset_a, offset_b, out);
        recurse(
            &a[middle..],
            &b[split..],
            offset_a + middle,
            offset_b + split,
            out,
        );
    }

    let mut out = Vec::with_capacity(a.len().min(b.len()));
    recurse(a, b, 0, 0, &mut out);
    out
}

/// Semantic IL diff: LCS over canonical line keys, with cosmetic-only changes
/// reported as `rename` and genuine changes as `replace`. `replace`/`rename`
/// lines also carry token-level spans for inline highlighting.
pub fn diff_il(req: &IlDiffRequest) -> IlDiff {
    diff_il_lines(&req.a.lines, &req.b.lines, &req.rename_map, &req.addr_map)
}

pub(crate) fn diff_il_lines(
    lines_a: &[IlLine],
    lines_b: &[IlLine],
    rename_map: &HashMap<String, String>,
    addr_map: &HashMap<String, String>,
) -> IlDiff {
    let variables_a = variable_map_for_lines(lines_a);
    let variables_b = variable_map_for_lines(lines_b);
    diff_il_lines_with_variables(
        lines_a,
        lines_b,
        rename_map,
        addr_map,
        &variables_a,
        &variables_b,
    )
}

/// Block detail must use the same function-wide variable identities as block
/// matching. Rebuilding these maps for each block hides cross-block alias changes.
pub(crate) fn diff_il_lines_with_variables(
    lines_a: &[IlLine],
    lines_b: &[IlLine],
    rename_map: &HashMap<String, String>,
    addr_map: &HashMap<String, String>,
    variables_a: &VariableMap,
    variables_b: &VariableMap,
) -> IlDiff {
    let empty_map = HashMap::new();
    let keys_a: Vec<String> = lines_a
        .iter()
        .map(|line| canonical_line(line, rename_map, addr_map, variables_a))
        .collect();
    let keys_b: Vec<String> = lines_b
        .iter()
        .map(|line| canonical_line(line, &empty_map, &empty_map, variables_b))
        .collect();

    let n = keys_a.len();
    let m = keys_b.len();

    // Preserve the original full-matrix tie behavior for ordinary functions.
    // Larger decompiler output uses exact Hirschberg alignment, avoiding the
    // quadratic allocation without falling back to an approximate positional diff.
    const MAX_LCS_CELLS: usize = 4_000_000;
    if n.checked_mul(m).is_none_or(|cells| cells > MAX_LCS_CELLS) {
        let matches = linear_memory_lcs_matches(&keys_a, &keys_b);
        let lcs = matches.len();
        let mut aligned = Vec::with_capacity(n + m - lcs);
        let (mut next_a, mut next_b) = (0usize, 0usize);
        for (matched_a, matched_b) in matches {
            aligned.extend((next_a..matched_a).map(Aligned::Delete));
            aligned.extend((next_b..matched_b).map(Aligned::Insert));
            aligned.push(
                if lines_a[matched_a].text.trim() == lines_b[matched_b].text.trim() {
                    Aligned::Equal(matched_a, matched_b)
                } else {
                    Aligned::Rename(matched_a, matched_b)
                },
            );
            next_a = matched_a + 1;
            next_b = matched_b + 1;
        }
        aligned.extend((next_a..n).map(Aligned::Delete));
        aligned.extend((next_b..m).map(Aligned::Insert));
        pair_replacements(&mut aligned);
        let similarity = lcs as f64 / n.max(m) as f64;
        return IlDiff {
            lines: materialize_lines(
                lines_a,
                lines_b,
                rename_map,
                addr_map,
                variables_a,
                variables_b,
                aligned,
            ),
            similarity,
        };
    }

    // LCS DP over canonical keys.
    let mut dp = vec![vec![0usize; m + 1]; n + 1];
    for i in (0..n).rev() {
        for j in (0..m).rev() {
            dp[i][j] = if keys_a[i] == keys_b[j] {
                dp[i + 1][j + 1] + 1
            } else {
                dp[i + 1][j].max(dp[i][j + 1])
            };
        }
    }
    let lcs = dp[0][0];

    // Backtrack into aligned ops. Unmatched A/B lines are first emitted as
    // delete/insert, then adjacent delete+insert runs are paired into `replace`
    // so changed lines show side by side.
    let mut aligned: Vec<Aligned> = Vec::new();
    let (mut i, mut j) = (0usize, 0usize);
    while i < n && j < m {
        if keys_a[i] == keys_b[j] {
            // Same canonical form: equal if rendered text matches, else a
            // cosmetic rename (var renumber / matched-callee rename).
            let same = lines_a[i].text.trim() == lines_b[j].text.trim();
            aligned.push(if same {
                Aligned::Equal(i, j)
            } else {
                Aligned::Rename(i, j)
            });
            i += 1;
            j += 1;
        } else if dp[i + 1][j] >= dp[i][j + 1] {
            aligned.push(Aligned::Delete(i));
            i += 1;
        } else {
            aligned.push(Aligned::Insert(j));
            j += 1;
        }
    }
    while i < n {
        aligned.push(Aligned::Delete(i));
        i += 1;
    }
    while j < m {
        aligned.push(Aligned::Insert(j));
        j += 1;
    }

    pair_replacements(&mut aligned);

    // Materialize aligned ops into DiffLines, computing token spans for the
    // changed (replace/rename) lines.
    let lines = materialize_lines(
        lines_a,
        lines_b,
        rename_map,
        addr_map,
        variables_a,
        variables_b,
        aligned,
    );

    let denom = n.max(m);
    let similarity = if denom == 0 {
        1.0
    } else {
        lcs as f64 / denom as f64
    };

    IlDiff { lines, similarity }
}

/// Compute the exact similarity reported by `diff_il_lines` without building
/// alignment operations or token spans. This is used by the graph endpoint,
/// which displays the score but already obtains per-block line detail
/// separately.
pub(crate) fn semantic_similarity(
    lines_a: &[IlLine],
    lines_b: &[IlLine],
    rename_map: &HashMap<String, String>,
    addr_map: &HashMap<String, String>,
) -> f64 {
    let variables_a = variable_map_for_lines(lines_a);
    let variables_b = variable_map_for_lines(lines_b);
    let empty_map = HashMap::new();
    let keys_a: Vec<String> = lines_a
        .iter()
        .map(|line| canonical_line(line, rename_map, addr_map, &variables_a))
        .collect();
    let keys_b: Vec<String> = lines_b
        .iter()
        .map(|line| canonical_line(line, &empty_map, &empty_map, &variables_b))
        .collect();
    let (n, m) = (keys_a.len(), keys_b.len());
    let denominator = n.max(m);
    if denominator == 0 {
        return 1.0;
    }

    // Similarity needs only the LCS length, so two rolling rows produce the
    // same score as the full alignment matrix with much less allocation.
    let (rows, columns) = if m <= n {
        (&keys_a, &keys_b)
    } else {
        (&keys_b, &keys_a)
    };
    let mut previous = vec![0usize; columns.len() + 1];
    let mut current = vec![0usize; columns.len() + 1];
    for i in (0..rows.len()).rev() {
        for j in (0..columns.len()).rev() {
            current[j] = if rows[i] == columns[j] {
                previous[j + 1] + 1
            } else {
                previous[j].max(current[j + 1])
            };
        }
        std::mem::swap(&mut previous, &mut current);
        current.fill(0);
    }
    previous[0] as f64 / denominator as f64
}

fn materialize_lines(
    lines_a: &[IlLine],
    lines_b: &[IlLine],
    rename_map: &HashMap<String, String>,
    addr_map: &HashMap<String, String>,
    variables_a: &VariableMap,
    variables_b: &VariableMap,
    aligned: Vec<Aligned>,
) -> Vec<DiffLine> {
    let span_line = |idx_a: usize, idx_b: usize| {
        token_spans_with_maps(
            &lines_a[idx_a],
            &lines_b[idx_b],
            rename_map,
            addr_map,
            variables_a,
            variables_b,
        )
    };
    aligned
        .into_iter()
        .map(|a| match a {
            Aligned::Equal(ia, ib) => DiffLine {
                op: DiffOp::Equal,
                a: Some(lines_a[ia].text.clone()),
                b: Some(lines_b[ib].text.clone()),
                a_spans: Vec::new(),
                b_spans: Vec::new(),
            },
            Aligned::Rename(ia, ib) => {
                let (sa, sb) = span_line(ia, ib);
                DiffLine {
                    op: DiffOp::Rename,
                    a: Some(lines_a[ia].text.clone()),
                    b: Some(lines_b[ib].text.clone()),
                    a_spans: sa,
                    b_spans: sb,
                }
            }
            Aligned::Replace(ia, ib) => {
                let (sa, sb) = span_line(ia, ib);
                DiffLine {
                    op: DiffOp::Replace,
                    a: Some(lines_a[ia].text.clone()),
                    b: Some(lines_b[ib].text.clone()),
                    a_spans: sa,
                    b_spans: sb,
                }
            }
            Aligned::Delete(ia) => DiffLine {
                op: DiffOp::Delete,
                a: Some(lines_a[ia].text.clone()),
                b: None,
                a_spans: Vec::new(),
                b_spans: Vec::new(),
            },
            Aligned::Insert(ib) => DiffLine {
                op: DiffOp::Insert,
                a: None,
                b: Some(lines_b[ib].text.clone()),
                a_spans: Vec::new(),
                b_spans: Vec::new(),
            },
        })
        .collect()
}

/// Token-level LCS between two lines over their canonical token forms; returns
/// (a_spans, b_spans) where a token is `changed` if it is not part of the
/// common subsequence. Drives inline highlighting of just the changed tokens.
#[cfg(test)]
fn token_spans(
    line_a: &IlLine,
    line_b: &IlLine,
    rename_map: &HashMap<String, String>,
    addr_map: &HashMap<String, String>,
) -> (Vec<TokenSpan>, Vec<TokenSpan>) {
    let variables_a = variable_map_for_lines(std::iter::once(line_a));
    let variables_b = variable_map_for_lines(std::iter::once(line_b));
    token_spans_with_maps(
        line_a,
        line_b,
        rename_map,
        addr_map,
        &variables_a,
        &variables_b,
    )
}

fn token_spans_with_maps(
    line_a: &IlLine,
    line_b: &IlLine,
    rename_map: &HashMap<String, String>,
    addr_map: &HashMap<String, String>,
    variables_a: &VariableMap,
    variables_b: &VariableMap,
) -> (Vec<TokenSpan>, Vec<TokenSpan>) {
    // Keep every token (so concatenating span texts reproduces the line
    // verbatim, preserving spacing/punctuation); canonical form drives the
    // match. Whitespace tokens canonicalize to "" and match harmlessly.
    let roles_a = token_roles(line_a);
    let roles_b = token_roles(line_b);
    let empty_map = HashMap::new();
    let ta: Vec<(&IlToken, String)> = line_a
        .tokens
        .iter()
        .zip(roles_a)
        .map(|(t, role)| {
            (
                t,
                canonical_token(t, rename_map, addr_map, variables_a, role),
            )
        })
        .collect();
    let tb: Vec<(&IlToken, String)> = line_b
        .tokens
        .iter()
        .zip(roles_b)
        .map(|(t, role)| {
            (
                t,
                canonical_token(t, &empty_map, &empty_map, variables_b, role),
            )
        })
        .collect();

    let (na, nb) = (ta.len(), tb.len());
    const MAX_TOKEN_LCS_CELLS: usize = 65_536;
    if na
        .checked_mul(nb)
        .is_none_or(|cells| cells > MAX_TOKEN_LCS_CELLS)
    {
        let keys_a: Vec<&str> = ta.iter().map(|(_, canonical)| canonical.as_str()).collect();
        let keys_b: Vec<&str> = tb.iter().map(|(_, canonical)| canonical.as_str()).collect();
        let matches = linear_memory_lcs_matches(&keys_a, &keys_b);
        let mut matched_a = vec![false; na];
        let mut matched_b = vec![false; nb];
        for (a, b) in matches {
            matched_a[a] = true;
            matched_b[b] = true;
        }
        let spans = |tokens: &[(&IlToken, String)], matched: &[bool]| {
            tokens
                .iter()
                .enumerate()
                .map(|(index, (token, canonical))| TokenSpan {
                    text: token.text.clone(),
                    changed: !matched[index] && !canonical.is_empty(),
                })
                .collect()
        };
        return (spans(&ta, &matched_a), spans(&tb, &matched_b));
    }
    let mut dp = vec![vec![0usize; nb + 1]; na + 1];
    for i in (0..na).rev() {
        for j in (0..nb).rev() {
            dp[i][j] = if ta[i].1 == tb[j].1 {
                dp[i + 1][j + 1] + 1
            } else {
                dp[i + 1][j].max(dp[i][j + 1])
            };
        }
    }

    let mut sa = Vec::with_capacity(na);
    let mut sb = Vec::with_capacity(nb);
    let (mut i, mut j) = (0usize, 0usize);
    while i < na && j < nb {
        if ta[i].1 == tb[j].1 {
            sa.push(TokenSpan {
                text: ta[i].0.text.clone(),
                changed: false,
            });
            sb.push(TokenSpan {
                text: tb[j].0.text.clone(),
                changed: false,
            });
            i += 1;
            j += 1;
        } else if dp[i + 1][j] >= dp[i][j + 1] {
            // Don't emphasize whitespace/punctuation-only tokens (empty canonical).
            sa.push(TokenSpan {
                text: ta[i].0.text.clone(),
                changed: !ta[i].1.is_empty(),
            });
            i += 1;
        } else {
            sb.push(TokenSpan {
                text: tb[j].0.text.clone(),
                changed: !tb[j].1.is_empty(),
            });
            j += 1;
        }
    }
    while i < na {
        sa.push(TokenSpan {
            text: ta[i].0.text.clone(),
            changed: !ta[i].1.is_empty(),
        });
        i += 1;
    }
    while j < nb {
        sb.push(TokenSpan {
            text: tb[j].0.text.clone(),
            changed: !tb[j].1.is_empty(),
        });
        j += 1;
    }
    (sa, sb)
}

/// Collapse each run of consecutive deletes-then-inserts into paired `replace`
/// ops (with any leftover left as plain insert/delete), so a changed line
/// renders as old||new rather than two separate hunks.
fn pair_replacements(aligned: &mut Vec<Aligned>) {
    let mut out: Vec<Aligned> = Vec::with_capacity(aligned.len());
    let mut k = 0;
    while k < aligned.len() {
        if matches!(aligned[k], Aligned::Delete(_)) {
            let del_start = k;
            while k < aligned.len() && matches!(aligned[k], Aligned::Delete(_)) {
                k += 1;
            }
            let ins_start = k;
            while k < aligned.len() && matches!(aligned[k], Aligned::Insert(_)) {
                k += 1;
            }
            let dels: Vec<usize> = aligned[del_start..ins_start]
                .iter()
                .map(|a| {
                    if let Aligned::Delete(i) = a {
                        *i
                    } else {
                        unreachable!()
                    }
                })
                .collect();
            let inss: Vec<usize> = aligned[ins_start..k]
                .iter()
                .map(|a| {
                    if let Aligned::Insert(j) = a {
                        *j
                    } else {
                        unreachable!()
                    }
                })
                .collect();
            let paired = dels.len().min(inss.len());
            for p in 0..paired {
                out.push(Aligned::Replace(dels[p], inss[p]));
            }
            for &d in dels.iter().skip(paired) {
                out.push(Aligned::Delete(d));
            }
            for &ins in inss.iter().skip(paired) {
                out.push(Aligned::Insert(ins));
            }
        } else {
            // Move the non-delete op as-is.
            match aligned[k] {
                Aligned::Equal(a, b) => out.push(Aligned::Equal(a, b)),
                Aligned::Rename(a, b) => out.push(Aligned::Rename(a, b)),
                Aligned::Replace(a, b) => out.push(Aligned::Replace(a, b)),
                Aligned::Insert(b) => out.push(Aligned::Insert(b)),
                Aligned::Delete(a) => out.push(Aligned::Delete(a)),
            }
            k += 1;
        }
    }
    *aligned = out;
}

#[cfg(test)]
mod tests {
    use super::*;

    // Helpers to build IL lines as typed token streams.
    fn tok(kind: &str, text: &str) -> IlToken {
        IlToken {
            kind: kind.to_string(),
            text: text.to_string(),
        }
    }
    fn line(tokens: Vec<IlToken>, text: &str) -> IlLine {
        IlLine {
            tokens,
            text: text.to_string(),
        }
    }

    // The two functions from the spec, as HLIL token streams.
    // old:  if (true) / return 1 / j__guard_check_icall(nullptr) / return testa(arg1, zx.q(arg2), arg3)
    fn old_lines() -> Vec<IlLine> {
        vec![
            line(
                vec![
                    tok("keyword", "if"),
                    tok("text", "("),
                    tok("keyword", "true"),
                    tok("text", ")"),
                ],
                "if (true)",
            ),
            line(
                vec![tok("keyword", "return"), tok("integer", "1")],
                "    return 1",
            ),
            line(
                vec![
                    tok("codeSymbol", "j__guard_check_icall"),
                    tok("text", "("),
                    tok("keyword", "nullptr"),
                    tok("text", ")"),
                ],
                "j__guard_check_icall(nullptr)",
            ),
            line(
                vec![
                    tok("keyword", "return"),
                    tok("codeSymbol", "testa"),
                    tok("text", "("),
                    tok("localVariable", "arg1"),
                    tok("text", ", zx.q("),
                    tok("localVariable", "arg2"),
                    tok("text", "), "),
                    tok("localVariable", "arg3"),
                    tok("text", ")"),
                ],
                "return testa(arg1, zx.q(arg2), arg3)",
            ),
        ]
    }
    // new (replace): if (false) ... return testb(...)
    fn new_lines_replace() -> Vec<IlLine> {
        vec![
            line(
                vec![
                    tok("keyword", "if"),
                    tok("text", "("),
                    tok("keyword", "false"),
                    tok("text", ")"),
                ],
                "if (false)",
            ),
            line(
                vec![tok("keyword", "return"), tok("integer", "1")],
                "    return 1",
            ),
            line(
                vec![
                    tok("codeSymbol", "j__guard_check_icall"),
                    tok("text", "("),
                    tok("keyword", "nullptr"),
                    tok("text", ")"),
                ],
                "j__guard_check_icall(nullptr)",
            ),
            line(
                vec![
                    tok("keyword", "return"),
                    tok("codeSymbol", "testb"),
                    tok("text", "("),
                    tok("localVariable", "arg1"),
                    tok("text", ", zx.q("),
                    tok("localVariable", "arg2"),
                    tok("text", "), "),
                    tok("localVariable", "arg3"),
                    tok("text", ")"),
                ],
                "return testb(arg1, zx.q(arg2), arg3)",
            ),
        ]
    }

    #[test]
    fn replace_call_is_a_real_change() {
        // No rename map: testa->testb is a genuine replacement.
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: old_lines(),
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: new_lines_replace(),
            },
            rename_map: HashMap::new(),
            addr_map: HashMap::new(),
        };
        let d = diff_il(&req);
        // Line 0 (if true/false) and line 3 (testa/testb) are replaces;
        // lines 1 and 2 are equal.
        let replaces: Vec<&DiffLine> = d.lines.iter().filter(|l| l.op == DiffOp::Replace).collect();
        assert_eq!(
            replaces.len(),
            2,
            "if-condition and call should both be replaces: {:#?}",
            d.lines
        );
        assert!(d.lines.iter().any(|l| l.op == DiffOp::Replace
            && l.a.as_deref() == Some("return testa(arg1, zx.q(arg2), arg3)")
            && l.b.as_deref() == Some("return testb(arg1, zx.q(arg2), arg3)")));
    }

    #[test]
    fn renamed_call_is_cosmetic_not_a_replace() {
        // testa and testb are the SAME matched callee, just renamed across
        // builds. The condition flip is still a real change; the call is not.
        let mut rename_map = HashMap::new();
        rename_map.insert("testa".to_string(), "testb".to_string());
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: old_lines(),
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: new_lines_replace(),
            },
            rename_map,
            addr_map: HashMap::new(),
        };
        let d = diff_il(&req);
        // The call line now matches canonically -> reported as `rename`, not replace.
        let call = d
            .lines
            .iter()
            .find(|l| l.a.as_deref() == Some("return testa(arg1, zx.q(arg2), arg3)"))
            .unwrap();
        assert_eq!(
            call.op,
            DiffOp::Rename,
            "matched-callee rename must be cosmetic: {:#?}",
            d.lines
        );
        // Exactly one real change remains: the if-condition flip.
        let replaces: Vec<&DiffLine> = d.lines.iter().filter(|l| l.op == DiffOp::Replace).collect();
        assert_eq!(
            replaces.len(),
            1,
            "only the condition flip is a real change: {:#?}",
            d.lines
        );
    }

    #[test]
    fn callee_maps_apply_only_to_baseline_tokens() {
        let line = |kind: &str, target: &str| IlLine {
            text: format!("call {target}"),
            tokens: vec![
                IlToken {
                    kind: "keyword".into(),
                    text: "call ".into(),
                },
                IlToken {
                    kind: kind.into(),
                    text: target.into(),
                },
            ],
        };
        let compare = |kind: &str,
                       a: &str,
                       b: &str,
                       rename_map: HashMap<String, String>,
                       addr_map: HashMap<String, String>| {
            diff_il(&IlDiffRequest {
                a: IlFunction {
                    level: "HLIL".into(),
                    lines: vec![line(kind, a)],
                },
                b: IlFunction {
                    level: "HLIL".into(),
                    lines: vec![line(kind, b)],
                },
                rename_map,
                addr_map,
            })
        };
        let rename = HashMap::from([("foo".into(), "bar".into())]);
        assert_eq!(
            compare("codeSymbol", "foo", "bar", rename.clone(), HashMap::new()).lines[0].op,
            DiffOp::Rename
        );
        let changed = compare("codeSymbol", "bar", "foo", rename, HashMap::new());
        assert_eq!(changed.lines[0].op, DiffOp::Replace);
        assert!(changed.lines[0].a_spans[1].changed && changed.lines[0].b_spans[1].changed);

        let data_rename = HashMap::from([("foo".into(), "bar".into())]);
        assert_eq!(
            compare("dataSymbol", "foo", "bar", data_rename, HashMap::new()).lines[0].op,
            DiffOp::Replace
        );

        let chain = HashMap::from([("foo".into(), "bar".into()), ("bar".into(), "baz".into())]);
        assert_eq!(
            compare("codeSymbol", "foo", "bar", chain, HashMap::new()).lines[0].op,
            DiffOp::Rename
        );
        let address_map = HashMap::from([("0x1000".into(), "0x2000".into())]);
        assert_eq!(
            compare(
                "codeAddress",
                "0x2000",
                "0x1000",
                HashMap::new(),
                address_map
            )
            .lines[0]
                .op,
            DiffOp::Replace
        );
    }

    #[test]
    fn identical_il_is_fully_equal() {
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: old_lines(),
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: old_lines(),
            },
            rename_map: HashMap::new(),
            addr_map: HashMap::new(),
        };
        let d = diff_il(&req);
        assert!((d.similarity - 1.0).abs() < 1e-9);
        assert!(d.lines.iter().all(|l| l.op == DiffOp::Equal));
    }

    #[test]
    fn ssa_dup_suffix_is_cosmetic() {
        // `__xmm@..0303` vs `__xmm@..0303_1`: a Binary Ninja dup suffix only.
        let a = vec![line(
            vec![tok("keyword", "return"), tok("dataSymbol", "__xmm@aa0303")],
            "return __xmm@aa0303",
        )];
        let b = vec![line(
            vec![
                tok("keyword", "return"),
                tok("dataSymbol", "__xmm@aa0303_1"),
            ],
            "return __xmm@aa0303_1",
        )];
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: a,
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: b,
            },
            rename_map: HashMap::new(),
            addr_map: HashMap::new(),
        };
        let d = diff_il(&req);
        assert_eq!(
            d.lines[0].op,
            DiffOp::Rename,
            "dup suffix must be cosmetic: {:#?}",
            d.lines
        );
    }

    #[test]
    fn long_numeric_suffix_is_preserved() {
        // `Feature_1196105017` is a real identifier, not a dup suffix.
        let a = vec![line(
            vec![tok("codeSymbol", "Feature_1196105017")],
            "Feature_1196105017",
        )];
        let b = vec![line(vec![tok("codeSymbol", "Feature_42")], "Feature_42")];
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: a,
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: b,
            },
            rename_map: HashMap::new(),
            addr_map: HashMap::new(),
        };
        let d = diff_il(&req);
        // Neither identifier is a generated data symbol, so both remain
        // literal and the two differ.
        assert_eq!(
            d.lines[0].op,
            DiffOp::Replace,
            "long id must not collapse: {:#?}",
            d.lines
        );
    }

    #[test]
    fn graph_local_goto_target_renumbering_is_cosmetic() {
        let branch = |label_a: &str, address_a: &str, label_b: &str, address_b: &str| {
            let a = vec![line(
                vec![
                    tok("text", "goto "),
                    tok("gotoLabel", label_a),
                    tok("text", " @ "),
                    tok("codeRelativeAddress", address_a),
                ],
                &format!("goto {label_a} @ {address_a}"),
            )];
            let b = vec![line(
                vec![
                    tok("text", "goto "),
                    tok("gotoLabel", label_b),
                    tok("text", " @ "),
                    tok("codeRelativeAddress", address_b),
                ],
                &format!("goto {label_b} @ {address_b}"),
            )];
            let req = IlDiffRequest {
                a: IlFunction {
                    level: "MLIL".into(),
                    lines: a,
                },
                b: IlFunction {
                    level: "MLIL".into(),
                    lines: b,
                },
                rename_map: HashMap::new(),
                addr_map: HashMap::new(),
            };
            diff_il(&req)
        };

        let diff = branch("112", "0x1000493bc", "108", "0x10004a064");
        assert_eq!(
            diff.lines[0].op,
            DiffOp::Rename,
            "graph-local label and target-address churn must be cosmetic"
        );
        assert!(diff.lines[0]
            .a_spans
            .iter()
            .chain(&diff.lines[0].b_spans)
            .all(|span| !span.changed));
    }

    #[test]
    fn changed_code_address_is_a_real_change() {
        // A call target rendered as a literal code address: a different target
        // is a real change (kept literal), but a data address is collapsed.
        let a = vec![line(
            vec![tok("codeRelativeAddress", "0x1000"), tok("text", "(")],
            "0x1000(",
        )];
        let b = vec![line(
            vec![tok("codeRelativeAddress", "0x2000"), tok("text", "(")],
            "0x2000(",
        )];
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: a,
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: b,
            },
            rename_map: HashMap::new(),
            addr_map: HashMap::new(),
        };
        assert_eq!(
            diff_il(&req).lines[0].op,
            DiffOp::Replace,
            "changed code address must surface"
        );

        // Data/global addresses relocate; collapsed to ADDR -> cosmetic.
        let a = vec![line(
            vec![
                tok("keyword", "return"),
                tok("possibleAddress", "0x1801b8300"),
            ],
            "return 0x1801b8300",
        )];
        let b = vec![line(
            vec![
                tok("keyword", "return"),
                tok("possibleAddress", "0x1801c9400"),
            ],
            "return 0x1801c9400",
        )];
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: a,
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: b,
            },
            rename_map: HashMap::new(),
            addr_map: HashMap::new(),
        };
        assert_eq!(
            diff_il(&req).lines[0].op,
            DiffOp::Rename,
            "relocated data address must be cosmetic"
        );
    }

    #[test]
    fn relocated_matched_callee_is_cosmetic_via_addr_map() {
        // Same callee, different literal code address across builds. Without an
        // addr_map this is a real change; with the matcher's address pairing it
        // normalizes equal (cosmetic).
        let a = vec![line(
            vec![tok("codeRelativeAddress", "0x1000"), tok("text", "(")],
            "0x1000(",
        )];
        let b = vec![line(
            vec![tok("codeRelativeAddress", "0x2000"), tok("text", "(")],
            "0x2000(",
        )];
        let mut addr_map = HashMap::new();
        addr_map.insert("0x1000".to_string(), "0x2000".to_string());
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: a,
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: b,
            },
            rename_map: HashMap::new(),
            addr_map,
        };
        assert_eq!(
            diff_il(&req).lines[0].op,
            DiffOp::Rename,
            "relocated-but-matched call target must be cosmetic with an addr_map"
        );
    }

    #[test]
    fn replace_line_marks_only_changed_tokens() {
        // `return testa(arg1)` vs `return testb(arg1)`: only the call symbol
        // changed; the token spans must flag exactly that token.
        let a = vec![line(
            vec![
                tok("keyword", "return"),
                tok("codeSymbol", "testa"),
                tok("text", "("),
                tok("localVariable", "arg1"),
                tok("text", ")"),
            ],
            "return testa(arg1)",
        )];
        let b = vec![line(
            vec![
                tok("keyword", "return"),
                tok("codeSymbol", "testb"),
                tok("text", "("),
                tok("localVariable", "arg1"),
                tok("text", ")"),
            ],
            "return testb(arg1)",
        )];
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: a,
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: b,
            },
            rename_map: HashMap::new(),
            addr_map: HashMap::new(),
        };
        let d = diff_il(&req);
        let l = &d.lines[0];
        assert_eq!(l.op, DiffOp::Replace);
        let changed_a: Vec<&str> = l
            .a_spans
            .iter()
            .filter(|s| s.changed)
            .map(|s| s.text.as_str())
            .collect();
        let changed_b: Vec<&str> = l
            .b_spans
            .iter()
            .filter(|s| s.changed)
            .map(|s| s.text.as_str())
            .collect();
        assert_eq!(
            changed_a,
            vec!["testa"],
            "only the call symbol changed on A: {:#?}",
            l.a_spans
        );
        assert_eq!(
            changed_b,
            vec!["testb"],
            "only the call symbol changed on B: {:#?}",
            l.b_spans
        );
    }

    #[test]
    fn variable_renumbering_is_cosmetic() {
        let a = vec![line(
            vec![tok("keyword", "return"), tok("localVariable", "var_10")],
            "return var_10",
        )];
        let b = vec![line(
            vec![tok("keyword", "return"), tok("localVariable", "var_18")],
            "return var_18",
        )];
        let req = IlDiffRequest {
            a: IlFunction {
                level: "MLIL".into(),
                lines: a,
            },
            b: IlFunction {
                level: "MLIL".into(),
                lines: b,
            },
            rename_map: HashMap::new(),
            addr_map: HashMap::new(),
        };
        let d = diff_il(&req);
        assert_eq!(d.lines[0].op, DiffOp::Rename);
    }

    #[test]
    fn generated_variable_alias_change_is_semantic() {
        let assignment = |left: &str, right: &str| {
            line(
                vec![
                    tok("localVariable", left),
                    tok("text", " = "),
                    tok("localVariable", right),
                ],
                &format!("{left} = {right}"),
            )
        };
        let returned = |name: &str| {
            line(
                vec![tok("keyword", "return"), tok("localVariable", name)],
                &format!("return {name}"),
            )
        };
        let req = IlDiffRequest {
            a: IlFunction {
                level: "MLIL".into(),
                lines: vec![assignment("var_8", "var_10"), returned("var_8")],
            },
            b: IlFunction {
                level: "MLIL".into(),
                lines: vec![assignment("var_18", "var_20"), returned("var_20")],
            },
            ..Default::default()
        };
        let diff = diff_il(&req);
        assert!(diff.similarity < 1.0);
        assert!(diff.lines.iter().any(|line| line.op == DiffOp::Replace));
    }

    #[test]
    fn changed_integer_bound_is_semantic() {
        let mk = |bound: &str| {
            vec![line(
                vec![
                    tok("keyword", "if"),
                    tok("text", " ("),
                    tok("localVariable", "len"),
                    tok("text", " > "),
                    tok("integer", bound),
                    tok("text", ")"),
                ],
                &format!("if (len > {bound})"),
            )]
        };
        let req = IlDiffRequest {
            a: IlFunction {
                level: "MLIL".into(),
                lines: mk("64"),
            },
            b: IlFunction {
                level: "MLIL".into(),
                lines: mk("128"),
            },
            ..Default::default()
        };
        let d = diff_il(&req);
        assert_eq!(d.lines[0].op, DiffOp::Replace);
        assert_eq!(d.similarity, 0.0);
    }

    #[test]
    fn meaningful_variable_swap_is_semantic() {
        let mk = |left: &str, right: &str| {
            vec![line(
                vec![
                    tok("localVariable", left),
                    tok("text", " = "),
                    tok("localVariable", right),
                ],
                &format!("{left} = {right}"),
            )]
        };
        let req = IlDiffRequest {
            a: IlFunction {
                level: "MLIL".into(),
                lines: mk("dst", "src"),
            },
            b: IlFunction {
                level: "MLIL".into(),
                lines: mk("src", "dst"),
            },
            ..Default::default()
        };
        assert_eq!(diff_il(&req).lines[0].op, DiffOp::Replace);
    }

    #[test]
    fn meaningful_short_numeric_symbol_suffix_is_preserved() {
        let mk = |name: &str| vec![line(vec![tok("codeSymbol", name)], name)];
        let req = IlDiffRequest {
            a: IlFunction {
                level: "HLIL".into(),
                lines: mk("AES_128"),
            },
            b: IlFunction {
                level: "HLIL".into(),
                lines: mk("AES_256"),
            },
            ..Default::default()
        };
        assert_eq!(diff_il(&req).lines[0].op, DiffOp::Replace);
    }

    #[test]
    fn meaningful_numeric_variable_suffix_is_semantic() {
        let returned = |name: &str| {
            line(
                vec![tok("keyword", "return"), tok("localVariable", name)],
                &format!("return {name}"),
            )
        };
        let req = IlDiffRequest {
            a: IlFunction {
                lines: vec![returned("buffer_128")],
                ..Default::default()
            },
            b: IlFunction {
                lines: vec![returned("buffer_256")],
                ..Default::default()
            },
            ..Default::default()
        };
        let diff = diff_il(&req);
        assert_eq!(diff.similarity, 0.0);
        assert_eq!(diff.lines[0].op, DiffOp::Replace);
        assert!(diff.lines[0]
            .a_spans
            .iter()
            .any(|span| span.changed && span.text == "buffer_128"));
    }

    #[test]
    fn generated_argument_duplicate_suffix_remains_cosmetic() {
        let req = IlDiffRequest {
            a: IlFunction {
                lines: vec![line(vec![tok("localVariable", "arg1")], "arg1")],
                ..Default::default()
            },
            b: IlFunction {
                lines: vec![line(vec![tok("localVariable", "arg1_1")], "arg1_1")],
                ..Default::default()
            },
            ..Default::default()
        };
        let diff = diff_il(&req);
        assert_eq!(diff.similarity, 1.0);
        assert_eq!(diff.lines[0].op, DiffOp::Rename);
    }

    #[test]
    fn generated_argument_indices_remain_distinct() {
        let mk = |name: &str| vec![line(vec![tok("localVariable", name)], name)];
        let req = IlDiffRequest {
            a: IlFunction {
                lines: mk("arg_1"),
                ..Default::default()
            },
            b: IlFunction {
                lines: mk("arg_2"),
                ..Default::default()
            },
            ..Default::default()
        };
        assert_eq!(diff_il(&req).lines[0].op, DiffOp::Replace);
        let renamed = IlDiffRequest {
            b: IlFunction {
                lines: mk("arg1_1"),
                ..Default::default()
            },
            ..req
        };
        assert_eq!(diff_il(&renamed).lines[0].op, DiffOp::Rename);
    }

    #[test]
    fn very_large_diff_uses_exact_bounded_alignment() {
        let lines: Vec<IlLine> = (0..2001)
            .map(|i| line(vec![tok("integer", &i.to_string())], &i.to_string()))
            .collect();
        let req = IlDiffRequest {
            a: IlFunction {
                level: "MLIL".into(),
                lines: lines.clone(),
            },
            b: IlFunction {
                level: "MLIL".into(),
                lines,
            },
            ..Default::default()
        };
        let diff = diff_il(&req);
        assert_eq!(diff.similarity, 1.0);
        assert!(diff.lines.iter().all(|line| line.op == DiffOp::Equal));
    }

    #[test]
    fn very_large_shift_retains_exact_line_coverage() {
        let lines: Vec<IlLine> = (0..2001)
            .map(|i| line(vec![tok("integer", &i.to_string())], &i.to_string()))
            .collect();
        let mut shifted = vec![line(vec![tok("integer", "inserted")], "inserted")];
        shifted.extend(lines.clone());
        let req = IlDiffRequest {
            a: IlFunction {
                level: "MLIL".into(),
                lines,
            },
            b: IlFunction {
                level: "MLIL".into(),
                lines: shifted,
            },
            ..Default::default()
        };
        let diff = diff_il(&req);
        assert_eq!(
            diff.lines
                .iter()
                .filter(|line| line.op == DiffOp::Equal)
                .count(),
            2001
        );
        assert_eq!(
            diff.lines
                .iter()
                .filter(|line| line.op == DiffOp::Insert)
                .count(),
            1
        );
        assert!((diff.similarity - 2001.0 / 2002.0).abs() < 1e-12);
    }

    #[test]
    fn very_large_token_shift_retains_exact_token_coverage() {
        let tokens_a: Vec<IlToken> = (0..257).map(|i| tok("integer", &i.to_string())).collect();
        let mut tokens_b = vec![tok("integer", "inserted")];
        tokens_b.extend(tokens_a.clone());
        let line_a = line(tokens_a, "a");
        let line_b = line(tokens_b, "b");
        let (a, b) = token_spans(&line_a, &line_b, &HashMap::new(), &HashMap::new());
        assert!(a.iter().all(|span| !span.changed));
        assert_eq!(b.iter().filter(|span| span.changed).count(), 1);
        assert!(b[0].changed);
    }

    #[test]
    fn score_only_path_matches_full_diff() {
        let req = IlDiffRequest {
            a: IlFunction {
                level: "MLIL".into(),
                lines: vec![
                    line(vec![tok("text", "a")], "a"),
                    line(vec![tok("text", "b")], "b"),
                    line(vec![tok("text", "c")], "c"),
                ],
            },
            b: IlFunction {
                level: "MLIL".into(),
                lines: vec![
                    line(vec![tok("text", "a")], "a"),
                    line(vec![tok("text", "x")], "x"),
                    line(vec![tok("text", "c")], "c"),
                ],
            },
            ..Default::default()
        };
        assert_eq!(
            semantic_similarity(&req.a.lines, &req.b.lines, &req.rename_map, &req.addr_map,),
            diff_il(&req).similarity
        );
    }
}
