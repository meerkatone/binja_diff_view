# Accuracy and performance fix plan

This document retains the schema-20 planning baseline below. Later schemas and
performance batches supersede parts of that baseline; see
[schema 22 validation](schema22-validation.md) for later matching corrections.

Status: correctness work implemented through match schema 20 and extraction
schema 11; calibration, corpus, and output-preserving performance work remain.

Schema 20 replaces unsafe heuristic must-pair promotion with binding unique
exact/name seeds and complete neighbor evidence. Assignment enforces those seeds;
inferred matches do not recursively validate other matches. It adds ordered
instruction-pair evidence, zero scoring for missing bodies, a conservative
ambiguity cap, and bidirectional fuzzy retrieval over complete equal-size groups.
Extracted string references now participate in retrieval and comprehensive scores.
Selected-function work runs in a worker, and the table preserves selection and
sorting while exposing anchor evidence and combined filters. The earlier schema
history and benchmark values below describe their original versions, not schema 20.
Confidence calibration against independently labeled data remains open.

Earlier implementations included unique-only name fallback, operand-aware body evidence, typed
address normalization, full-function sequence scoring, generated-local alpha
identity, maximum-cardinality/maximum-weight ambiguity assignment, separation
of name evidence from body similarity, shared immutable overlay cache payloads,
fixed-point matched-neighbor evidence, honest native-algorithm documentation,
and versioned matching through schema 19. Schema 15 added post-match
security-product triage without changing pair assignment. Schema 16 persists
baseline/patched direction and extraction schema 11 resolves string evidence
through code, data-wrapper, and supported derived-string references. Schema 17
restores Conflicting rows to Relevant and excludes generated WPP trace-GUID
churn from the maintenance veto. The new behavior is covered by focused
regression tests.
Schema 18 requires two direct mechanics for one-hop propagation and filters weak
Possible rows out of Relevant without hiding them from the full result.
Schema 19 kept support/contradiction ties out of the fixed-point `must` set;
schema 20 supersedes that promotion mechanism.
Patch-signature schema 5 preserves whole-block insert/delete evidence, counts
confirmed absence as a classified patch-region observation, and rejects stale
signatures that treated graph-local goto labels and branch addresses as patch
constants.

Still planned: a labelled golden corpus, confidence calibration, candidate-recall
measurement and further retrieval indexing, corpus/FFI profiling, score-profile labels throughout all
export surfaces, and optional
compatibility profiles. Those items need measured ground truth or a separate
compatibility and licensing decision; they should not be guessed into the
matcher.

A three-pair BinDiff 8 comparison baseline was recorded on 2026-08-31. It adds
external mapping and score evidence for `screensharingd`, `nwifi.sys`, and
CVE-2024-38063, but it is not the labelled golden corpus described below.

A fresh schema-16 `spoolsv` rematch was recorded on 2026-09-01. Native and
BinDiff both accepted 2,332 mappings and agreed on 2,331 (99.9571%). This is
strong comparison evidence for that pair, but the single disagreement still
needs an independent label and the run does not replace held-out ground truth.

## Decision and scope

Binja Diff View's current matcher is a custom `rust_diff`-derived engine. It
uses ideas that also appear in BinDiff and Diaphora, but it is not a compatible
implementation of either tool. Different scores are therefore expected.

The first objective is better match accuracy and clearer score semantics, not
forcing one native number to resemble two unrelated external numbers. BinDiff
and Diaphora do not calculate the same thing, so one score cannot be compatible
with both. Any future compatibility profile must be a separate, versioned
output and must pass parity tests before it is called compatible.

This plan covers:

- correctness and accuracy findings from the plugin review;
- performance changes that must preserve match and score output exactly;
- an audit of the local algorithms against current BinDiff and Diaphora;
- validation, rollout, and cache-version requirements;
- optional external score profiles after the native matcher is trustworthy.

## Upstream sources checked

The comparison was made against the current official repositories on
2026-08-29:

- [BinDiff source](https://github.com/google/bindiff)
- [BinDiff matching concepts](https://github.com/google/bindiff/blob/main/docs/concepts.md)
- [BinDiff exact similarity and confidence code](https://github.com/google/bindiff/blob/main/differ.cc#L362-L537)
- [BinDiff default matching steps and confidence values](https://github.com/google/bindiff/blob/main/bindiff.json#L24-L166)
- [BinDiff mnemonic signature code](https://github.com/google/bindiff/blob/main/prime_signature.h)
- [BinDiff flow-graph signature construction](https://github.com/google/bindiff/blob/main/flow_graph.cc#L228-L295)
- [Diaphora source](https://github.com/joxeankoret/diaphora)
- [Diaphora candidate heuristics](https://github.com/joxeankoret/diaphora/blob/master/diaphora_heuristics.py)
- [Diaphora per-function ratio code](https://github.com/joxeankoret/diaphora/blob/master/diaphora.py#L1512-L1634)
- [Diaphora MD-index extraction](https://github.com/joxeankoret/diaphora/blob/master/diaphora_ida.py#L2511-L2538)
- [Diaphora KOKA CFG hash](https://github.com/joxeankoret/diaphora/blob/master/jkutils/graph_hashes.py)

Diaphora is AGPL-3.0 and BinDiff is Apache-2.0. Exact Diaphora compatibility
work needs a licensing decision before code is copied or derived closely from
its implementation. The audit itself does not copy upstream code.

## Why the scores differ

### Native Binja Diff View score

The native function score in `rust_engine/src/algorithms.rs` is a dynamically
renormalized weighted average of available evidence:

| Evidence | Nominal weight |
|---|---:|
| CFG equality or structural counts | 0.30 |
| call degree | 0.20 |
| basic-block mnemonic multiset | 0.15 |
| normalized instruction/operand multiset and adjacent-pair overlap, equally weighted | 0.15 |
| edge-count similarity | 0.10 |

The final fuzzy phase mixes that score at 60% with a second score at 40%. The
second score uses structural counts, call degree, constants, strings, and a
full mnemonic sequence edit distance. Informative names are separate pairing
evidence and do not contribute to reported body similarity. Confidence is then
assigned by a local affine formula for each phase, capped at 0.5 for non-binding
pairs whose local ambiguity margin is below 0.01. Whole-binary similarity is
mean accepted-pair similarity multiplied by function coverage.

These are native definitions. None is the BinDiff or Diaphora definition.

### BinDiff

Current BinDiff computes function similarity from actual matched flow-graph
edges, basic blocks, and instructions. In the source, those quotas receive
weights 0.55, 0.30, and 0.15; the result is averaged with MD-index similarity
and then multiplied by confidence. A special case returns 1.0 directly when all
basic blocks and instructions match. Otherwise, confidence is derived from a
histogram of the function and basic-block matching steps, averaged using
configured method confidence values, then passed through a sigmoid.

Its whole-binary score separately weights matched non-library flow-graph edges,
basic blocks, functions, instructions, and call-graph MD-index similarity. It is
not an average of per-function similarities.

The local plugin does not currently retain all of the matched-edge,
matched-block, matching-step histogram, library, and call-graph data needed to
reproduce that score.

### Diaphora

Diaphora first generates candidates using dozens of ordered SQL heuristics. Its
per-function `check_ratio()` then returns 1.0 for an identical bytes hash or
takes the maximum of cleaned pseudocode, cleaned assembly, pseudocode-prime/AST,
MD-assisted, and cleaned microcode ratios. Small tie-breaking bonuses use
features such as compilation unit, AST primes, degree, switches, complexity,
and shared constants. Same-name handling can also promote a match.

The local plugin has no IDA bytes-hash pipeline, Hex-Rays pseudocode AST,
Hex-Rays microcode, SQL heuristic suite, or equivalent cleanup functions. Its
score should not be expected to track Diaphora's ratio.

## Algorithm compatibility audit

| Area | Local implementation | BinDiff | Diaphora | Finding |
|---|---|---|---|---|
| Candidate strategy | Seven ordered phases, then a bounded size-window fuzzy pass | Recursive unique-attribute matching, call-graph drill-down, call-reference propagation, and a long configurable function/basic-block step list | Dozens of ordered SQL heuristics plus call-graph and brute-force passes | Native strategy; not compatible |
| Function score | Weighted feature similarity; fuzzy phase is `0.6 * primary + 0.4 * comprehensive` | Matched edge/block/instruction quotas, MD-index distance, and step confidence | Maximum cleaned assembly/pseudocode/AST/microcode ratio with MD rules and small bonuses | Scores have different meanings |
| Confidence | Per-phase affine function of similarity | Histogram of function and block methods, configured values, then sigmoid | Result buckets and heuristic-specific ratio rules; no equivalent local confidence formula | The former `BinDiff-style` comment was misleading and has been removed |
| Whole-binary score | Mean matched similarity times function coverage | Separate non-library edge/block/function/instruction/call-graph formula | Reports match buckets and coverage; it does not use the local aggregate | Numbers are not comparable |
| MD index | Reverse-postorder rank with five irrational terms, rounded to eight decimals; leaf/empty values are address-salted | Top-down, bottom-up, and relaxed graph MD attributes supplied by the BinDiff graph pipeline | SCC topological order with `order + in(src)*sqrt(2) + out(src)*sqrt(3) + in(dst)*sqrt(5) + out(dst)*sqrt(7)`, summed as reciprocal square roots using `Decimal` | Local formula is not Diaphora's and is not BinDiff-compatible |
| Mnemonic signature | Pair-local sorted mnemonic-to-consecutive-prime map; product modulo `2^61-1` | Stable modified Goedel mnemonic IDs; current flow-graph signature sums instruction IDs | Fixed architecture instruction list mapped to primes; arbitrary-precision product | All three implementations differ |
| CFG hash | Three-round labeled Weisfeiler-Lehman-style hash | Multiple graph attributes and matching algorithms; not this WL hash | KOKA prime-feature product over nodes, edges, calls, references, loops, SCCs, and function flags | The comment claiming Diaphora equivalence is incorrect |
| Basic blocks | Canonical-IL line similarity, unique hashes, CFG propagation, then greedy remaining matches | Configurable edge, hash, prime, call/string reference, MD, dominator, loop, entry/exit, count, and jump-sequence algorithms | Exported CFG/instruction data and UI diffing, driven by function candidate heuristics | Local block matching is native |

### Consequence

There is no evidence that the local formulas are transcription bugs from
BinDiff or Diaphora; provenance identifies `meerkatone/rust_diff` as the bundled
upstream. At audit time, several comments implied compatibility that the code
did not provide; schema 8 removes those claims. The native algorithms should be
evaluated on ground-truth precision and recall rather than numeric proximity to
either external tool.

The local `Exact` key also means exact under native normalization, not BinDiff
or Diaphora equivalence: it combines operand-retaining instruction content, the
local WL hash, and caller/callee counts. Before match schema 8, meaningful-name
edit distance also affected the native body score. Schema 8 keeps name evidence
for pairing while reporting body likeness separately.

## Accuracy and correctness work

### P0: establish a reproducible accuracy baseline

Before further matching changes:

1. Add a versioned golden-corpus manifest containing known A-to-B function
   mappings. Include:
   - identical and relocation-only builds;
   - stripped versus symbolized binaries;
   - reordered functions and blocks;
   - compiler and optimization-level changes;
   - duplicate/template/thunk-heavy code;
   - operand-only security changes;
   - functions longer than 512 instructions;
   - changed local-variable aliasing;
   - independently labelled historical mispair examples.
2. Record versioned native matches, similarity, confidence, method, unmatched
   rows, runtime, and peak memory. Use schema 12 for the current baseline and
   preserve a pre-schema-8 snapshot from commit `94ca649` where practical.
3. Where possible, export BinDiff and Diaphora results for the same ground-truth
   corpus. Treat their results as comparison evidence, not truth.
4. Measure match precision, recall, F1, top-candidate margin, and calibration.
   A score change is accepted only when it improves ground-truth metrics or
   fixes a specified correctness case without unacceptable regressions.
5. Add symmetry and determinism tests: reversing A/B must invert the same
   mapping, and extraction order or thread scheduling must not change results.

The 2026-08-31 seed comparison covers three real binary pairs.
Accepted address mappings agree on 1,741 of 1,878 `screensharingd` pairs, 1,003
of 1,004 `nwifi.sys` pairs, and all 5,581 CVE-2024-38063 pairs. Every named
mapping agrees. All remaining disagreements are generated functions, so they
need independent labels before they can contribute to precision or recall.

The 2026-09-01 schema-16 `spoolsv` validation adds a fourth exact comparison.
BinDiff and Native each accept 2,332 pairs and agree on 2,331. The sole
disagreement is `YAsyncGetRemoteNotifications`: BinDiff selects
`AcquireChannel`, while Native selects the name-aligned
`AsyncGetNotifications` through call-graph evidence. Neither target is treated
as ground truth.

### P0: make score semantics honest and versioned

Partially complete. Misleading compatibility comments were removed, the native
body score no longer includes name edit distance, cache versions reject stale
matching and canonicalization results, and pair artifacts retain component
scores and directional security identities. Export-wide profile labels,
component explanations in corpus JSON/CSV and standalone Rust output, and
Native v1 golden fixtures remain open.

1. Rename displayed and serialized scores to `Native similarity`, `Native
   confidence`, and `Native overall` where ambiguity exists.
2. Remove or qualify claims such as `BinDiff-style`, `Diaphora SPP`, and
   `Diaphora graph_hashes.py equivalent` in code and documentation.
3. Export component scores and the scoring profile/version with every match.
   Users should be able to see why a pair scored as it did.
4. Put algorithm and score versions in persistent pair, extraction, corpus, and
   patch-signature cache keys. Bump the affected schemas whenever matching,
   canonicalization, or scoring changes.
5. Preserve Native v1 golden output as a regression fixture. Native v2 may
   intentionally differ, but every difference must be attributable.

### P0: remove unsafe implicit name pairing

Resolved in schema 8. `src/diffcontroller.cpp::functionByName()` now rejects
generated identities and requires the raw or normalized identity to be unique
on both sides. It no longer returns the first matching function.

Implementation:

- use the accepted address map by default;
- permit fallback only when the normalized identity is unique on both sides;
- otherwise show an ambiguous/unmatched state and require an explicit manual
  choice;
- test duplicate short names, duplicate raw names, overloads, namespaces,
  imports, and thunks.

### P0: preserve variable identity during IL canonicalization

Resolved in schema 8. Generated `var_<hex>` locals receive stable per-function
alpha identities in order of first semantic use. Stack-layout renumbering stays
cosmetic, while changed equality and alias relationships affect the diff.

Implementation:

- assign deterministic per-function alpha-renamed IDs according to stable
  variable/storage identity and first semantic use;
- preserve equality relationships within the function;
- normalize corresponding stack offsets only through an explicit mapping, not
  by collapsing all locals;
- add tests for pure stack-layout changes, swapped source/destination locals,
  merged/split variables, and SSA versions.

### P0: make long-function sequence scoring representative

Resolved in schema 8. Instruction-sequence similarity covers the complete
function. It removes common prefixes and suffixes before exact rolling-row edit
distance, preserving the score while reducing work for localized patches.

Implementation:

- replace prefix truncation with an exact scalable sequence algorithm, or use a
  deterministic whole-function representation with a proven error bound;
- retain fast equality and length checks;
- add adversarial tests where the common prefix is identical and all changes
  occur after instruction 512.

### P0: prevent mnemonic-only evidence from implying exactness

Resolved in schema 8. SmallPrimes remains a mnemonic-only candidate key, but
accepted candidates are classified with operand-sensitive body evidence.
Binary Ninja address tokens normalize relocation, while integer tokens stay
literal regardless of magnitude.

Schema 9 also normalizes Binary Ninja's generated address-derived
`CodeSymbolToken` and `DataSymbolToken` names. Meaningful code/data symbols and
integer tokens remain literal. On the measured `tcpip.sys` pair, this rule
resolved 851 of 861 structurally identical operand-only rows in the schema-8
result. The earlier claim that a full schema-9 replay produced one changed
function was based on a stale engine replay and is withdrawn. The installed
schema-9 plugin showed 16 changed matches and two functions added on the patched
side.

Schema 10 fixes the remaining classification error by separating pairing score
from content state. Extraction schema 7 drops analysis annotations and types a
memory displacement as relocatable only when it resolves into a BinaryView
section. Rows are now `Matched`, `Cosmetic`, or `Changed`; the Changed filter
uses that state rather than a `0.999` similarity threshold. The schema-10
Personal replay produced 5,581 matches, two functions present only in the
patched build, and seven Changed rows. Comparing those rows with BinDiff exposed
six extraction false positives.

Schema 11 / extraction schema 8 addressed two defects found by comparing the
`tcpip.sys` result with BinDiff. Annotation tokens can occur between machine
operand tokens, so extraction skipped the annotation instead of truncating the
rest of the instruction. For an integer inside a memory operand, it also added
the displayed literal as a fallback when the token's semantic value did not
resolve into the image. The Personal replay confirmed that the three truncated
functions became exact, but it also showed eight Changed rows: section lookup
did not recognize the image-relative offsets, and annotation payload tokens
still leaked into content.

The Personal UI replay showed that skipping only `AnnotationToken` was still
insufficient: the renderer emits its stack-variable, possible-value, and string
payload as separate tokens. Schema 12 / extraction schema 9 excludes those
analysis-only tokens using their token kind and operand metadata, while
resuming at real operands after an inline annotation. Mapped-image displacement
checks also use BinaryView segments, with sections retained as a fallback.

The final schema-12 replay used Binary Ninja Personal 6.1.10552-dev through the
UI MCP backend, with both BNDBs clean and idle. It produced 5,581 matches, no
vulnerable-only functions, and two patched-only functions. The matched rows
were 1,915 Exact, 3,665 Cosmetic, and one Changed. The sole Changed function was
`Ipv6pProcessOptions`: 199 to 207 instructions, 779 to 799 decoded bytes, 86.1%
native similarity, and 98.6% native confidence. The match cardinality and all
5,581 address pairs agree with the imported BinDiff result. The **Byte size
changed** filter also selects only this function for this pair; it remains a
triage signal because equal byte counts do not establish content equality and
different byte counts do not establish a semantic change.

Implementation:

- keep mnemonic signatures only as candidate generators;
- always rerank and classify accepted candidates with operand-sensitive or
  canonical-IL evidence;
- reserve 1.0 for evidence that establishes semantic equality under the
  documented normalization rules;
- add immediate, bound, mask, call-target, displacement, and register/data-flow
  change fixtures.

### P1: improve ambiguous one-to-one assignment

Resolved for normal ambiguity components in schema 8. Connected components up
to 128 functions per side use maximum-cardinality, maximum-weight assignment.
Larger components retain a deterministic greedy fallback.

Implementation:

- build small connected ambiguity components from candidate edges;
- solve maximum-weight bipartite assignment within each component;
- retain deterministic address/index tie-breaks;
- keep unambiguous singleton and unique-key matches on the fast path;
- cap pathological component size and report when a documented fallback is
  used.

Implementation tests cover the greedy counterexample, strict cardinality, and
A/B reversal for the selected component. Golden-corpus precision and recall
acceptance remains open.

### P1: improve candidate recall without an unbounded Cartesian product

Schema 20 ranks complete equal-size groups plus nearby sizes, using instruction
content, extracted strings, constants, and shape. It retrieves 48 candidates
per function in each direction and scores the union, removing the previous
index-based truncation of large equal-size groups. Size-changing compiler
transformations can still move counterparts outside retrieval, and large groups
still require linear cheap ranking per function.

Remaining work:

- generate a union of candidates from independent indexes: size bands, CFG
  features, constants, strings/imports, call neighborhoods, mnemonic sketches,
  and meaningful names;
- widen adaptively when the best score has a small margin or no candidate
  clears threshold;
- use reciprocal-nearest or mutual-best evidence as a precision signal, not a
  substitute for assignment;
- log candidate counts and pruning reasons for auditability.

### P1: strengthen feature evidence and calibration

Partially complete. Schema 8 separates meaningful-name evidence from reported
body similarity. Extraction schema 11 adds code, data-wrapper, and supported
derived-string references, while schema 15 applies explicit contradiction and
direct-evidence rules to security triage. Confidence is still not calibrated,
and export explanations remain incomplete.

The security UI predicates must remain distinct from evidence extraction.
`Strong` has not changed since schema 15. Schema 16 narrows `Relevant` from the
old raw-score threshold to `Strong` or `Possible`, excluding `Conflicting`.
Richer schema-11 evidence can nevertheless promote rows from `Unknown`, so the
labelled corpus must measure review-queue precision and noise as well as match
precision. The current `spoolsv` run has zero `Strong`, 22 `Possible`, and two
`Conflicting` rows. Schema 16 showed 22 under Relevant; schema 17 restores the
two Conflicting rows to that review queue.

The `nwifi.sys` fixture showed why that reduction was not automatically an
improvement. Its schema-15 and schema-16 assessments are identical, but the
known `Dot11Translate80211ToEthernetNdisPacket` change is `Conflicting` at
0.445 and therefore disappeared from Relevant. A generated WPP `Traceguids`
rename supplies the maintenance counter-signal. Schema 17 resolves the
review-recall regression in two places: generated WPP trace identifiers no
longer count as maintenance-only vocabulary, and Relevant includes
`Conflicting` as an explicit review path. The schema-16 artifacts remain a
regression fixture.
Across today's three schema-16 pair artifacts, the state-based filter omits six
Conflicting rows: one in `nwifi.sys`, three in SMBFS, and two in `spoolsv`.
Schema 17 restores all six to the review queue. Independent labels are still
needed to measure queue precision and recall.

Schema 18 addresses the separate Possible-row noise seen in curl 8.3.0 to
8.4.0. The exact replay keeps all 1,416 mappings and reduces Relevant from 50
rows to 17. The known `nwifi.sys` row and all four known `screensharingd` rows
remain. This is fixture evidence, not calibrated precision or recall.

1. Separate `similarity` (body likeness) from `confidence` (probability that the
   pairing is correct). Do not make confidence an arbitrary affine restatement
   of similarity.
2. Remove symbol-name edit distance from body similarity. Use names as candidate
   and pairing-confidence evidence, and report rename evidence separately.
3. Calibrate confidence from held-out ground truth by method, function size,
   ambiguity, score margin, and feature availability.
4. Add rare constants, string/data references, import/callee identity,
   cyclomatic complexity, SCC/loop features, and call-neighborhood agreement
   where extraction is reliable.
5. Penalize contradictory evidence. Dynamic renormalization must not let a
   sparse pair score highly merely because difficult features are absent.
6. Retain component values and explanations in exports and tooltips.

### P1: harden normalization and collision handling

Partially complete. Whole-binary extraction now transports Binary Ninja operand
token types, so address normalization no longer guesses from integer magnitude.
The remaining collision and multiplicity work is still open.

- replace lossy name/address textual normalization with structured tokens;
- distinguish code, data, relocation, integer, stack, and symbol operands;
- preserve multiplicity for constants and references where it matters;
- never rely on a compact hash alone for a final match: verify the underlying
  canonical feature vector on hash equality;
- make empty and single-block functions explicitly ineligible for structural
  evidence instead of address-salting a purported position-independent MD
  value.

## Performance work that must not change accuracy

Every item in this section has a hard acceptance condition: for the complete
golden corpus and randomized fixtures, accepted mappings, methods, component
scores, similarities, confidences, unmatched sets, and deterministic ordering
must be bit-for-bit identical before and after the optimization.

### P1: stop copying overlay cache entries

Resolved. Overlay cache entries and active views share immutable extracted
functions and block-diff results. Cache hits no longer copy graph payloads.

Implementation:

- store immutable entries behind `std::shared_ptr<const OverlayCacheEntry>` or
  an equivalent stable handle;
- move newly computed entries into the cache once;
- keep LRU mutation under the lock but use immutable payloads outside it;
- measure hit latency, retained bytes, and peak transient memory.

### P1: cache canonical IL preprocessing

Open.

Block matching and line/token diffing repeatedly normalize, tokenize, clone,
and hash the same selected-function lines.

Fix:

- create one immutable preprocessed representation per extracted function and
  IL/cache key;
- reuse canonical lines, token vectors, hashes, and block metadata for block
  candidates, line LCS, token spans, and whole-function scoring;
- preserve the existing invalidation boundary and eight-entry LRU behavior.

### P1: use lossless candidate pruning

Open. No exhaustive-oracle parity harness exists yet.

- index exact and keyed phases once by their full verified keys;
- compute mathematically valid upper bounds for weighted scores and skip a pair
  only when its maximum possible score is below threshold;
- order cheap discriminators before expensive edit distances;
- retain a test mode that runs both pruned and exhaustive scoring and asserts
  identical results.

No heuristic top-K reduction belongs in this performance phase; candidate caps
are accuracy policy and must be evaluated separately.

### P2: reduce corpus recomputation and copying

Partially complete. Pair results are retained by shared immutable reference and
rapid family updates are coalesced. Combined filter accumulation, incremental
family/cell recomputation, and cold/warm 45-edge performance budgets remain.

- combine score filters and directional metric accumulation into one pass;
- update only families and matrix cells affected by an arriving pair;
- retain immutable pair results by shared reference rather than copying maps;
- benchmark cold and warm reference, lineage, and 45-edge all-pairs runs;
- assert identical manifests, exports, family membership, and classifications.

### P2: reduce Rust/C++ serialization overhead

Partially complete. Extraction avoids some redundant renderer fields and keeps
one structured instruction stream, but no stage-level FFI profile, compact
versioned encoding, or typed ABI exists. The JSON FFI still duplicates large
strings and nested structures.

Fix in stages:

1. measure extraction, serialization, parse, match, and result-transfer time
   separately;
2. remove redundant fields and repeated strings without changing semantics;
3. add string interning or compact typed arrays behind a versioned envelope;
4. consider a typed ABI only after ownership, cancellation, compatibility, and
   malformed-input tests exist.

Keep the JSON path as a parity oracle until the replacement produces identical
results.

## Optional external score profiles

These are separate projects, not prerequisites for Native v2.

### BinDiff-like profile

A defensible BinDiff-like estimate requires:

- final matched block and instruction correspondences for every function;
- matched CFG edge counts;
- compatible MD-index and call-graph MD-index values;
- function and block matching-step histograms;
- library classification;
- BinDiff's configured confidence values and sigmoid.

Until all inputs and parity fixtures exist, label this `BinDiff-like estimate`,
not `BinDiff score`. Exact parity may still be impossible when Binary Ninja and
BinExport produce different function boundaries, CFGs, instructions, or names.
An importer for actual BinDiff output is more honest when exact BinDiff numbers
are required.

### Diaphora-inspired profile

A Diaphora-like ratio requires compatible cleaned assembly, decompiler
pseudocode, AST-prime, microcode, bytes-hash, MD-index, and tie-break features,
plus its candidate heuristic behavior. Binary Ninja IL is not Hex-Rays
pseudocode or microcode, so a native implementation should be called
`Diaphora-inspired` unless it achieves fixture parity on equivalent inputs.

Resolve AGPL obligations before implementing or porting Diaphora code. An
importer for Diaphora databases/results avoids presenting Binary Ninja-derived
features as equivalent IDA-derived values.

## Outstanding work after schema 20

| Priority | Work | Current state |
| --- | --- | --- |
| P0 | Labelled golden corpus and held-out precision, recall, F1, calibration, symmetry, and parallel determinism measurements | Open. Four BinDiff comparisons are useful external evidence, not independent labels. |
| P0 | Score semantics on every surface | Partial. Pair artifacts carry components and versions; corpus JSON/CSV, standalone Rust output, and some UI wording still need explicit Native profile identifiers and explanations. |
| P1 | Adaptive fuzzy candidate recall | Partial in schema 20. Complete equal-size groups, content/string/constant ranking, and bidirectional retrieval replace the index-truncated window. Held-out recall measurements remain open. |
| P1 | Confidence and security-queue calibration | Partial. Schemas 17 and 18 fix the `nwifi.sys` recall regression and curl propagation noise; labelled noise and recall measurements remain open. |
| P1 | Normalization and collision completion | Partial. Structured operand types exist; multiplicity, full-vector verification on compact-hash equality, and explicit empty/single-block eligibility remain. |
| P1 | Canonical selected-function preprocessing and lossless pruning | Open. Both require exact-output or exhaustive-oracle parity gates. |
| P2 | Corpus recomputation and 45-edge budgets | Partial. Shared pair references and update coalescing exist; combined accumulation, incremental recomputation, and cold/warm runtime and memory budgets remain. |
| P2 | FFI profiling and compact transport | Open beyond initial field cleanup. Keep JSON as the parity oracle. |
| Optional | BinDiff-like and Diaphora-inspired profiles | Deferred pending feature parity, fixtures, naming, and licensing decisions. |

## Remaining implementation order

1. Golden corpus, metrics, symmetry tests, and pre-schema-8 snapshots where
   practical.
2. Export-wide native score labels, component explanations, and algorithm
   profile identifiers.
3. Adaptive candidate generation and confidence calibration.
4. Output-preserving preprocessing, pruning, corpus, and FFI optimizations with
   measured parity.
5. Optional BinDiff-like or Diaphora-inspired profiles, each independently
   gated by parity evidence and licensing review.

## Release gates

Native v2 is ready only when:

- no known wrong first-name fallback remains;
- operand-only and aliasing changes cannot be classified as identical;
- long-function tail changes affect the relevant score;
- match precision improves on the historical mispair corpus with no material
  recall regression;
- A/B reversal and repeated parallel runs produce identical inverse mappings;
- every performance-only commit passes exact output-parity tests;
- runtime and peak memory are no worse than the documented budget on the
  ten-binary/45-pair corpus;
- score profile and algorithm versions appear in UI details, JSON, CSV,
  manifests, and persistent cache keys;
- documentation no longer claims BinDiff or Diaphora equivalence without a
  passing compatibility suite.

Current gate audit:

- Passed: unsafe first-name fallback, operand-only and alias-sensitive content
  handling, long-function tail scoring, cache versioning, directional security
  identity, and non-equivalence documentation.
- Partial: deterministic component assignment tests, component telemetry, and
  historical BinDiff comparison evidence.
- Open: independently labelled precision/recall targets, full-pipeline A/B
  symmetry and repeated parallel-run tests, exhaustive parity gates, the
  ten-binary/45-pair performance budget, and profile identifiers on every UI
  and export surface.
