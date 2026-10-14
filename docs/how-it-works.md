# How Binja Diff View works

Binja Diff View has a native C++/Qt frontend and a bundled Rust matching engine.
C++ owns Binary Ninja objects, UI state, background work, rendering, and symbol
changes. Rust receives JSON snapshots and returns deterministic matches and
diffs.

```text
Binary A + Binary B
        │
        ├─ native extraction → Rust function matcher → results table
        │
selected function pair
        │
        └─ selected-IL extraction → one Rust block/line/token diff → render overlay
```

Matching and overlays are read-only. Only an approved symbol-port operation
changes a BinaryView.

## Corpus orchestration

The corpus workspace adds a serial scheduler around the existing pair matcher.
It does not add a second matching algorithm or depend on Binary Ninja's
Ultimate-only similarity API.

The current implementation supports three plans. Reference mode compares one
anchor with every target. Ordered lineage compares adjacent releases and can
add first-reference edges. Bounded all-pairs mode compares each unordered pair
once, with a default limit of 45 comparisons for ten binaries.

```text
selected open BinaryViews
        │
        └─ deterministic comparison plan
                    │
                    └─ serial pair scheduler
                              ├─ pair cache hit
                              └─ existing Rust function matcher
                                        │
                                        └─ directional matrix
                                                  │
                                                  └─ existing pair workspace
```

Reference mode emits one directed edge from the chosen reference to each
target. The scheduler probes the directional pair cache before starting an edge.
A cold edge calls the same cancellable `DiffEngine::matchFunctions` entry point
used by the pair workspace. It publishes each result independently, so one
failure does not remove earlier cells.

For accepted matches from row A to column B, function coverage is the number of
matched A functions divided by all A functions. Instruction coverage weights
the accepted source functions by extracted instruction count. Reversing the
matrix cell swaps those denominators and reuses the completed mapping metrics.
Opening that reversed cell starts or restores a separate B-to-A assessment
because security evidence is directional. Mean similarity and confidence
include accepted matches only. These values describe matching evidence, not
vulnerability evidence.

Reference mode builds one family per reference function and adds accepted target
occurrences to that anchor. Lineage and all-pairs modes sort links by confidence,
similarity, match-method strength, and stable address ties. A merge fails if it
would add a second occurrence from one binary. Heuristic all-pairs links must be
mutual best. Rejected links stay attached to the affected families, and strong
collisions mark them conflicting.

The setup dialog naturally sorts release filenames before showing them. Numeric
runs compare by value, common binary and database suffixes do not affect the
order, and prerelease markers sort before the matching final release. The user
can reapply the sort or drag rows to override it. Lineage mode preserves that
visible order. Adjacent lineage edges run first. An optional overlay adds
first-build comparisons without duplicating edges. All-pairs mode emits one
orientation per unordered pair and rejects plans above 45 pairs.

Matrix score filters are separate from pair matching. Changing a minimum size,
stub, thunk, generated-name, or exact-match filter recalculates directional
metrics from the immutable pair result. It does not extract or match again. Raw
coverage remains in each cell tooltip.

An arriving pair updates only its own matrix and reuse rows. The workspace uses
a short single-shot timer to combine rapid family updates, then performs one
final family rebuild when the scheduler finishes. This matters on warm
all-pairs runs, where dozens of cache hits can arrive within milliseconds.

Clean corpora use a small manifest keyed by the ordered persistent BinaryView
identities and mode. A `QSaveFile` write stores the payload with a SHA-256
digest. The manifest records plan edges, states, scoring settings, family
thresholds, patch thresholds, and optional patch metadata. Reference choice and
lineage overlay are part of the manifest key. It does not duplicate
`FunctionMatchMaps`. Reopening the same clean corpus loads the manifest settings
and restores every completed edge through the pair cache. Patch metadata has a
separate signature schema plus extraction and match schema versions. A mismatch
drops the signature and its classifications while preserving the other manifest
settings.

Corpus JSON and CSV exports capture the displayed generation and run in a
background task. Later pair arrivals do not change that captured export. Only
one export runs per workspace; cancel it from Binary Ninja's background task
list, or close the workspace. Families must have finished rebuilding before an
export can start.

Exports stream through a 64 KiB buffer into `QSaveFile`, which publishes the
destination after a successful final cancellation check. Cancellation or a
write failure leaves an existing destination intact. JSON builds one family
record at a time; CSV writes rows directly. This avoids a second complete JSON
tree and complete output strings, while retaining the captured family data and
manifest. JSON whitespace and field order may differ; the exported schema and
values are unchanged.

Corpus drill-down keeps the same snapshot boundary. Before opening a matrix,
family, or reuse pair, the workspace checks that the retained result is still
the exact object in the pair cache. It then captures both view epochs for the
new tab. A cache replacement, rematch, or view update causes the drill-down to
stop with an instruction to open a new corpus workspace.

The scheduler records every pair in microseconds. The workspace measures total
time through the final family rebuild. A 50 ms timer samples Binary Ninja's
resident memory during scheduling, and the workspace samples once more after
building families. The manifest and JSON export retain the starting and peak
values, so cold and warm runs can be compared without a separate profiler.

Patch signatures start from two explicitly confirmed occurrences in one family.
The plugin diffs them at HLIL when both views support it, falls back to MLIL,
and uses Disassembly only when neither semantic IL is available. It records
changed, removed, and added block addresses plus changed semantic lines, token
spans, call targets, constants, and data or string references. A target compares
against both anchors. The corpus scheduler runs
any missing anchor-to-target pairs before classification. Classification
requires evaluated patch-region coverage and a score margin. A confirmed
unmatched anchor block is evaluated as absent; extraction or mapping failures
remain missing coverage. Threshold changes reclassify cached targets, while
insufficient coverage remains ambiguous.
The same signature records an explainable security-relevance assessment over
fixed-side additions and vulnerable-side removals. Added guards, removed unsafe
memory operations, other memory or lifetime operations,
authorization logic, authentication negotiation, protocol state, success/error
semantics, identity/authority, capability, trust-boundary handling, and
numeric-risk expressions join monotonically; logging and diagnostics remain
counterevidence. The result is
`Unknown`, `Possible`, `Strong`, or `Conflicting`. A security-sensitive function
name is weak context and cannot move an otherwise empty assessment out of
`Unknown`. This is triage evidence, not an exploitability or vulnerability
classification.

Saving a BNDB refreshes every in-memory corpus pair that depends on that file
under its new persistent identity. Unrelated pair caches remain valid.

The current workspace accepts already-open analyzed views. Ten binaries are the
default all-pairs ceiling because they produce 45 matches. A measured cold-pair
FFmpeg run completed all 45 matches in 3.391 seconds with a 1,499.5 MiB sampled
resident-memory peak. That result does not justify an index or SQLite at the
current limit. The scheduler stays serial, and a higher limit still requires
new time and memory measurements.

After coalescing derived UI updates, a paired ten-binary run using identical
saved BNDB identities took 5.378 seconds cold and 187.737 ms warm. The warm run
restored all 45 edges from cache and reproduced the cold metrics and family
states exactly.

A five-binary ordered-lineage run used FFmpeg 7.0, 7.0.1, 7.0.2, 7.1, and
7.1.1. The first-reference overlay produced seven unique edges: four adjacent
comparisons and three additional reference comparisons. All seven cache hits
completed in 11.779 ms, and the full workspace completed in 51.306 ms. Every
raw and filtered pair metric matched the corresponding all-pairs edge. The
family builder produced 925 families without placing two functions from the
same binary in one family.

## Supported editions

The Personal Dev build owns the native plugin and corpus UI. Its build keeps the
normal `binja_diff_view` target and may refresh the installed development
plugin.

The older Commercial Dev build is a licensed automated headless backend. CMake
configures it with `BINJA_DIFF_BUILD_UI_PLUGIN=OFF`, so it builds the models,
persistence code, Rust engine, fixtures, and native Binary Ninja integration
test without building or installing the UI plugin. This also avoids treating
the Commercial build's older bundled Qt UI version as a supported plugin
runtime.

The Commercial suite validates deterministic corpus models, manifest and export
round trips, Rust matching, whole-binary matching, MLIL block extraction,
symbol-port undo and redo, cache persistence, and BNDB reopening. It loads only
bundled Binary Ninja plugins, so a Personal plugin compiled for another core ABI
cannot enter the headless process.

## Runtime layout

```text
plugins/binja_diff_view.dylib                C++/Qt Binary Ninja plugin
plugins/.binja_diff_view/librust_diff.dylib  private Rust FFI engine
```

The Rust library stays in the private directory because it is an FFI dependency,
not a standalone Binary Ninja plugin. The native plugin loads these entry points:

- `rust_diff_diff_json` — whole-binary function matching;
- `rust_diff_diff_json_cancellable` — the same matching with a cancellation
  callback polled between phases; optional, used when exported;
- `rust_diff_block_diff_json` — basic-block matching plus accepted-pair
  line/token detail and whole-function IL similarity;
- `rust_diff_il_diff_json` — semantic line and token changes;
- `rust_diff_free_string` — FFI result ownership.

The four entry points above other than the cancellable one are required. When
all four versioned snapshot exports are available, the native plugin uses
`rust_diff_prepare_json_v1`, `rust_diff_diff_prepared_v1`,
`rust_diff_prepared_size_v1`, and `rust_diff_free_prepared_v1` to retain immutable
parsed functions and matching features across pairs. Older engines continue
through the JSON entry point. Active comparisons share ownership of their
snapshots, so cache eviction cannot free a handle still in use.

The native library must match the installed Binary Ninja core ABI and bundled
Qt minor version.

## Workspace and direction

The workspace contains two native `ViewFrame` objects above a full-width results
table. The comparison panes and table can be hidden independently without
destroying their state. The persistent Graph diff / Linear selector changes both
native panes together; Space is synchronized through the same control. Returning
to Graph diff reopens the selected pair and restores its overlay. Both graphs
open at that pair rather than at each Linear cursor, which can differ between
panes. The workspace uses the active tab as baseline A. Linear view
projects the same block, line, and token state through Binary Ninja's native
render-layer callbacks, including whole-body HLIL. The shared IL selector
preserves the graph's existing form on open so workspace creation does not cause
lazy HLIL generation. It remains active in Linear mode and drives both native
Linear views, so changing IL there updates the rendered text and recomputes the
selected-function diff.

Each native pane owns a compact two-row toolbar. The first row contains Diff,
Rematch, change navigation, Clear, the IL form, and similarity scores. The
second row contains the overlay legend and compact changed/removed/added block
counts. Hover over those counts for the full function identities and scores;
the results-panel footer also retains the full summary. Both panes receive the
same completed summary, including when reopening a cached comparison. Status
text can wrap as the pane narrows, so duplicated function names do not force
the horizontal splitter wider.

Corpus transition cells display `Changed · similarity 98.4%`: the percentage is
the retained similarity score, not the proportion of instructions that changed.

Selected-function extraction and block/IL comparison run in a background worker
on cache misses. The worker owns core references and immutable snapshots; widget
access and overlay application remain on the event thread. Completion checks the
request generation, current functions, IL form, and cache revision before use.
Background Tasks exposes cancellation, checked between extraction/comparison
stages; an individual core extraction or Rust diff call may finish before it
observes cancellation.

The results table preserves selected address pairs, scroll position, and sorting
across refresh. Changed-only and Ambiguous-only checks combine with the dropdown
filter and text search. Right-click a column header to hide metric columns. The
Evidence toggle controls a selectable panel showing binding-anchor status and
neighbor-context support/contradiction counts. Full-row state highlighting uses
green for Matched, blue for Cosmetic, amber for Changed, and red for unmatched
functions, with white text.

The corpus workspace follows the same rule. Progress, run actions, score
filters, family filters, and patch thresholds are split into short rows.
Corpus selectors use `AdjustToMinimumContentsLengthWithIcon`, so the longest
binary name in a menu does not become the workspace minimum width.

All five corpus tables use `QHeaderView::ResizeToContents`. Qt measures every
model row rather than its default sample, so long function names outside the
initial rows still set the correct width. Model resets, result updates, and
proxy-filter changes recalculate the sections. The last section does not
stretch to fill the viewport; wide tables use per-pixel horizontal scrolling.

Side A is the reference and side B is the comparison:

- removed means present in A but not B;
- added means present in B but not A;
- instruction delta is `B - A`;
- symbol actions always state their transfer direction.

## Function matching

The native extractor records each function's name, address, instruction count,
CFG, mnemonics, operands, call relationships, and complexity. Following the
upstream `rust_diff` fix, matching uses Binary Ninja's valid qualified short
name rather than `Function.name`, a full name, or a rendered type signature.
This preserves class and namespace identity. Result rows and pair status text
use the final demangled identifier, so Windows C++ labels stay compact. PE/PDB
symbols whose short spelling is empty or still decorated fall back to
demangling the raw ABI name. Raw, full, and short spellings are retained
separately by the symbol-port path. Qualified match names remain searchable.
Function columns expose their displayed text and address as a tooltip when the
table width elides the visible value. Instructions are sent once inside their
owning blocks; Rust reconstructs the flat instruction view used by
whole-function metrics. Unused raw instruction bytes are not read or
transported. This whole-binary pass always uses disassembly, independent of the
visible IL.

Binary Ninja restores a database's functions lazily. Until something requests a
function's IL, its disassembly omits call-target symbols, data symbols, and
analysis annotations: `bl 0x1000749b8` instead of `bl sub_1000749b8`. A
database's extraction therefore depended on which functions had been viewed,
and a pair could differ in almost every call. Extraction schema 12 requests
each function's LLIL and renders the function while holding it, so every
function is extracted from the same analysis state. The core frees
unreferenced IL, so a separate prefetch pass would only generate it twice. Up to
eight workers extract functions concurrently and keep function-list order.
Freshly opened `screensharingd` 26.6 and 26.6.1 BNDBs (about 1,900 functions
each) extract and match in about seven seconds.

On AArch64 an address is built from a page base and a low immediate, as in
`adrp x3, page` then `add x3, x3, #0x97`. Moving a string or global changes both
without changing behavior:

- The `adrp` operand is a 4 KiB page base. Binary Ninja renders it as whatever
  symbol starts on that page, sometimes followed by an annotation of the string
  there. Extraction folds the operand into one page-address operand and does
  not collect strings through it.
- An immediate is extracted as an address when it equals the page offset of the
  instruction's resolved reference, or of the constant address its destination
  register holds afterwards (for a pointer consumed by a later instruction).
  That address must itself be mapped in the image; an image-base-relative
  reading does not count, since any small constant shares its own page offset.
  Only AArch64 uses this rule. Other immediates, including bounds, masks, and
  `__LINE__` arguments to logging calls, remain literal.

Function string context comes from each instruction's own code references
(except `adrp`), data-wrapper references, and derived strings. An integer is
also tried as an image-relative address only when that address lies in a
section; a small offset such as `[x8, #0x1c0]` otherwise lands in the Mach-O
header and collects load-command strings like `/usr/lib/dyld`. Rust preprocesses functions and immutable scoring features in
parallel, then reuses those features across every matching phase.

Rust accepts one-to-one pairs in ordered phases:

| Phase | Main evidence |
|---|---|
| Exact | instruction, CFG, and call fingerprints |
| Name | identical non-generated symbols |
| MdIndex | topological fingerprint plus similarity |
| SmallPrimes | mnemonic multiset fingerprint |
| Structural | non-trivial CFG hash |
| CallGraph | callers and callees of binding unique anchors |
| Heuristic | combined evidence for remaining functions |

SmallPrimes uses a native prime-product key over normalized mnemonic counts. The
key is case- and order-independent but preserves multiplicity. It only generates
candidates: final body similarity includes normalized operand content, so an
operand-only patch cannot remain 100% similar merely because its mnemonic bag is
unchanged. Operands that Binary Ninja explicitly marks as addresses are
normalized to avoid penalizing ordinary relocation. Integer displacements are
only treated as addresses when they occur inside a memory operand and resolve
to a section in the current BinaryView. Generated address-derived
`CodeSymbolToken` and `DataSymbolToken` names such as `sub_<address>` and
`data_<address>` normalize as well. Imported, debug, and analyst-provided
symbols remain literal, as do integer tokens regardless of magnitude. This
keeps named call-target changes, large bounds, and masks significant.
Confidence depends on the phase that accepted the pair, blended with the
pair's similarity `s`:

| Phase | Confidence |
|---|---|
| Exact, Manual | 100% |
| Name | 90% + 10% × s |
| MdIndex | 80% + 10% × s |
| Structural | 70% + 15% × s |
| CallGraph | 50% + 35% × s |
| SmallPrimes | 45% + 45% × s |
| Heuristic | 35% + 45% × s |

The SmallPrimes formula replaces an older narrow calibration that rounded
almost every strong SmallPrimes row to `85.0%`. Non-binding pairs with a local
ambiguity margin below 0.01 are capped at 50%.

Candidates within each connected ambiguity component are assigned for maximum
cardinality and then maximum pairing score. This prevents a locally strongest
edge from consuming the only counterpart available to another function.
Components larger than 128 functions per side use a deterministic greedy
fallback to bound worst-case work. Accepted functions cannot be reused by later
phases, and ties are resolved deterministically. Remaining functions become
A-only or B-only. Generated names such as `sub_1000`,
`nullsub_3`, and `fcn.00401000` are excluded from name evidence; if only one
binary is symbolized, its meaningful name does not penalize the body match.
Informative name similarity helps resolve ambiguous candidates but is reported
separately and does not change body similarity.

Instruction-sequence edit similarity covers the complete function rather than
only a fixed prefix, so changes after instruction 512 affect the score. IL
canonicalization assigns generated locals stable per-function alpha identities;
stack-slot renumbering stays cosmetic, while changed alias relationships remain
semantic.

The native MD-inspired key uses shortest-path depth from the function
entry instead of address-ordered DFS ranks. Edge contributions are sorted before
summation. Block permutations and branch relocation therefore preserve the key.
Missing entries use zero-in-degree roots, with a common level for unreachable
blocks. This key is not numerically compatible with BinDiff or Diaphora.

CFG fallback similarity combines count overlap with refined topology-label
histogram overlap; edge similarity compares endpoint-label pairs. Equal node
and edge counts alone no longer award perfect structural scores. These are
cached structural proxies, not established graph isomorphisms or BinDiff's
matched-edge ratios. Call similarity remains a degree proxy. Confidence remains
an uncalibrated method score; changing these formulas does not establish a
probability of correct correspondence or of a security fix.

### Algorithm boundaries

The matcher deliberately favors predictable runtime on large binaries:

- schema 24 introduced, and schema 25 retains, unique exact/name reservations and uses tentative
  neighborhood refinement for ranking. Mutually unique supported candidates
  extend a separate one-to-one context map. Each round reads a complete snapshot;
  context is pruned when support no longer exceeds contradictions or loses a
  path to a binding anchor. Rejected pairs cannot re-enter refinement. After
  assignment, any context pair rejected by the actual assignment is removed;
  dependents are pruned and assignment repeats against the smaller context set.
  Each retry strictly decreases that set. This is a terminating heuristic, not
  a monotone least-fixed-point solver. Tentative pairs never reserve endpoints.
  Neighbor counts are recomputed from the complete surviving maps. Key buckets
  are stored without their full cross-product; inverse neighbor lists nominate
  only supported pairs per round, and loops check cancellation. Dense connected
  duplicate buckets can still yield many candidates. Function indices
  are canonicalized by address before assignment so extraction order cannot
  decide ambiguous ties; already sorted inputs require no copy;
- ambiguity assignment is exact per connected component up to 128 functions per
  side, with a deterministic greedy fallback above that bound;
- fuzzy retrieval ranks the complete equal-size group plus 256 entries on either
  side using instruction content, extracted strings, constants, and shape.
  It retrieves 48 candidates in each direction and scores the union. Candidate
  ties use addresses rather than extraction indices. Rare shared strings also
  nominate up to 48 candidates in each direction outside the size shortlist,
  provided instruction-content overlap is at least 50%. Very large equal-size
  groups still require linear cheap ranking per function;
- unmatched functions can receive a review hint when a unique pair shares a
  reciprocal accepted caller and callee, at least one witness is binding, and
  their instruction content overlaps. Conflicting accepted neighbors and
  competing candidates suppress the hint. Hints never enter the function map,
  reported similarity, or security assessment;
- the strongest exact-content fingerprint retains meaningful symbol and integer
  operands while normalizing explicit addresses and generated address-derived
  symbols.

These limits affect match phase or recall, not the one-to-one invariant. Review
low-confidence and unmatched rows when comparing heavily reordered or relocated
binaries.

### Security triage

Security triage is a post-match projection: it never changes pair assignment,
reported similarity, or match confidence. Every changed or added function is
scored in a finite product lattice over direct patch-delta facts:

- Added and removed normalized instructions both count, and evidence labels
  record their direction. Side A is assumed to be the baseline and side B the
  patched build. Pair artifacts store both identities in that order, so a
  reversed workspace computes and caches its own assessment.
- String, data-symbol, and non-call code-symbol operands are masked when
  extracting direct operation deltas. Their text, and the extracted string
  references, supply context only. A string-only change cannot produce
  `Strong`.
- String references are resolved through code references, one level of data
  wrapping such as CFString objects, and derived-string xrefs where the API
  provides them, with renderer tokens as a fallback. Malformed UTF-8 is
  replaced rather than failing extraction.
- Path strings are compared with recognized build components masked. Apple
  `__FILE__` paths embed `BuildRoots/<random>/` and
  `TemporaryDirectory.<XXXXXX>/`, so every function using such a path
  previously gained added and removed string context from words like `Login`,
  `Message`, `Remote`, and `Roots` in the unchanged remainder. The component
  after `BuildRoots`, the suffix after `TemporaryDirectory.`, and hex digests of
  at least 16 digits (as in Nix or Bazel paths) are masked. Ordinary names stay
  literal, so `/etc/AuthV1Policy` → `/etc/UserV2Policy` still changes
  authentication and identity context. Strings without a path separator are
  never masked, and evidence labels come from the original text.
- Conditional x86 jumps and ARM condition branches count as guards; ARM bitwise
  operations and unconditional branches do not.
- Generated `WPP_<hex>_Traceguids` operands are treated as instrumentation
  identity churn, so they alone do not supply maintenance (logging)
  counter-evidence. Ordinary tracing changes still do.

Direct facts join one-hop call-neighbor context in a single pass. A function
accepts context only when it has at least two independent direct patch
mechanics; only direct facts transfer, and propagated context is never passed
on. The result is `Unknown`, `Possible`, `Strong`, or `Conflicting`:

- `Strong` requires direct security-semantic evidence.
- Ambiguous non-binding pairs are capped at `Possible`.
- **Security: relevant** shows every `Strong` and `Conflicting` row, plus
  `Possible` rows with a direct score of at least 0.39, or a direct score of at
  least 0.13 and a total score of at least 0.30. Pure context (a direct score
  of zero) is excluded.
- Byte-size filters are independent of security scores, so low-scoring
  structural changes remain reachable.

Patch signatures (signature schema 16) record whole-block insertions and
deletions as semantic lines and treat a confirmed absent anchor block as
evaluated zero-similarity evidence. They prefer HLIL over MLIL, retain line
addresses for navigation, and score patch blocks by diff index so structured
nodes can share a source address. Graph-local labels and generated jump-table
relocations are cosmetic; changed bounds and call targets remain semantic.

Security labels are heuristic triage, not vulnerability findings. The rules
above are the current ones; earlier schemas used different filters, recorded in
[schema and validation history](history.md).

### Scores and row state

- **Similarity** estimates how alike the function bodies and structures are.
  Schema 20 combines normalized instruction multisets with adjacent instruction
  pairs to retain operand order, without quadratic alignment for every pair.
  Missing body evidence contributes no score. This remains a similarity metric,
  not a test of semantic equivalence; content state is classified separately.
- **Confidence** is the native method score used to rank accepted pairings. It
  is not a calibrated probability or a BinDiff confidence value. In schema 20,
  non-binding pairs with a local ambiguity margin below 0.01 are capped at 0.5.
  This is a conservative ambiguity policy, not statistical calibration.
- **Margin** is the pairing-score distance to the strongest local alternative
  sharing either endpoint. Zero means the global assignment broke a tie.
- **Matched** means raw ordered instructions, CFG topology, and outgoing-call
  count agree.
- **Cosmetic** means raw rendering differs but the canonical ordered body, CFG,
  and outgoing-call count agree after relocation normalization.
- **Changed** means the canonical body, CFG, or outgoing-call count differs.
- **Overall** is matched similarity multiplied by coverage, so unmatched
  functions contribute zero.

Incoming-call count is pairing evidence, not content state: when another
function gains a call, the callee is not mislabeled as changed. The Changed
filter uses this content state directly and never infers it from similarity.

These are native Binja Diff View scores. BinDiff and Diaphora use different
matching inputs and formulas, so their numeric results are not expected to
agree. See the [algorithm audit and fix plan](accuracy-performance-fix-plan.md)
for the comparison and remaining calibration work.

Measured comparisons against BinDiff 8 and per-schema replays of the
`screensharingd`, `nwifi.sys`, `spoolsv`, and curl fixtures are recorded in
[schema and validation history](history.md). They compare behavior; they do not
establish either matcher as ground truth.

Rows are green for exact content, blue for cosmetic relocation differences,
amber for changed content, and red for unmatched functions. The dark
`rust_diff`-derived base colors
are brightened by 50%; active selection still uses Binary Ninja's native style.
Rows live in a `QAbstractTableModel` behind a sorting/filtering proxy, so search,
filter, and refresh do not create or hide one Qt widget item per function.
The evidence text and **Open in graphs** act on the selected row. When a search
or filter hides that row, Qt moves the current index to a remaining row without
selecting it; the panel then shows the selection prompt rather than that row's
evidence. The
compact metric sections use font- and style-derived initial widths that reserve
space for their labels, sort arrows, and representative cell values. Every
section uses interactive resize mode, including State, both function columns,
and every metric. The State section accounts for `Changed`/A-only/B-only, and
Match accounts for the named
matching phases plus `Unmatched`; the two function-name sections start at 300
pixels each when space is tight. On a wider viewport, they divide all width left
after the metric sections. The plugin then leaves every section in interactive
resize mode.

## Selected-function diff

Opening an accepted row navigates both graph panes to its stored addresses.
Binary Ninja may acknowledge navigation before the graph exposes its new current
function, so the controller waits until both panes report the requested starts.
A generation token cancels an older pending activation when the user opens a
new row or clears the diff. Only then does the plugin extract and overlay the
selected pair.

The selected extraction uses the shared visible IL: Disassembly, LLIL, MLIL,
HLIL, SSA forms, or a supported language representation such as Pseudo C.

Rust block matching:

1. pairs the entry blocks;
2. seeds unique canonical block hashes and propagates through the CFG;
3. pairs remaining blocks above `binjaDiffView.blockMatchThreshold`.

Paired blocks are equal, cosmetic, or changed. Unpaired A blocks are removed;
unpaired B blocks are added. A semantic line diff and token LCS refine changed
blocks. Those refinements and the whole-function IL score are returned with the
block correspondence, avoiding additional FFI/JSON calls for every changed
block. Ordinary line/token diffs retain the original full-matrix alignment;
large inputs switch to exact linear-memory Hirschberg alignment rather than an
approximate positional fallback. Independent block preprocessing and changed-pair
line diffs run in parallel; the whole-function score is computed alongside them.

Block status also compares outgoing CFG targets through the completed block
mapping, preserving edge multiplicity while ignoring block indices and edge
enumeration order. Retargeting an edge or inserting a guard marks its source
block changed even when the rendered tokens normalize equally. Similarity
remains a line-LCS score, so a CFG-only change can have 100% text similarity
and no changed token spans. Branch types are not yet included in this mapping.

Changed-block line and token detail reuse the same function-wide generated-local
identities as block matching. A changed use of a local established in another
block therefore remains visible in the inline highlights. Meaningful variable
names retain numeric suffixes; generated argument indices remain distinct,
while recognized generated duplicate suffixes stay cosmetic. Patch-signature
schema 16 rejects signatures computed before these fixes.

| Graph color | Meaning |
|---|---|
| Red `#BE2D32` | removed from A |
| Green `#289646` | added in B |
| Amber `#D7B41E` | semantically changed pair |
| Blue `#556EC8` | cosmetic-only difference |

Disassembly graphs apply the same `adrp` page and page-offset rules as the
whole-binary extraction, so a relocated string or global is cosmetic rather
than a red/green line pair. Tokens that follow an annotation without an operand
index, such as the string Binary Ninja shows for a page base, are analysis
display and are not compared. Linear disassembly leaves the empty separator
line after each block uncolored.

The overlay is keyed by BinaryView identity, function, block, line, and
instruction index. It remains in memory and never modifies analysis colors.
An entry-node function label can precede the extracted instruction lines; the
renderer resolves the block key among the displayed line addresses. Address
columns preserve their display tokens and consume only the extracted empty
separator span when present, so inline operand markers work with or without
displayed addresses and IL indices. Display-only linear indentation likewise
does not consume an engine span; an identical extracted indentation token keeps
its positional correspondence.

An eight-entry LRU retains exact extracted functions and block-diff output for
recent selections. Its key includes both views, function starts, IL form, and
the immutable match snapshot; analysis, symbol, and byte notifications evict
every affected entry.

## Synchronized navigation

While a diff is active:

- changing function opens its accepted counterpart and recomputes;
- changing IL updates both panes and recomputes;
- moving to a paired block moves the other pane to its counterpart;
- previous/next change visits changed, removed, and added sites.

Controller-generated navigation notifications are suppressed to prevent loops.
**Clear** cancels pending activation, stops synchronization, and removes the
overlay.

## Symbol porting

Symbol plans can use selected accepted rows, visible high-confidence rows, one
explicit A-only/B-only pair, or the functions currently displayed in the graph.
Before changing anything, the plugin previews source and target addresses and
rejects generated source names, protected target names, missing functions, and
duplicate targets.

An approved plan runs as one undoable transaction. A successful explicit
unmatched pair becomes an in-memory manual match. During the transaction,
symbol notifications invalidate extracted names without deleting the accepted
address mapping. Function and byte notifications still remove the whole pair.
At completion, the controller refreshes names only if the exact cache object
used to build the plan remains current. It never restores a pair removed by a
concurrent structural change. See
[Symbol porting and patch-diff testing](symbol-porting.md).

## Cache and persistence

Corpus metric calculation enumerates BinaryView functions only when thunk or
external-stub exclusions need symbol flags. Applying filters to completed pairs
captures those flags once per participating binary within that synchronous
application. The next application captures them again, so later symbol updates
are not hidden by a persistent metric cache. Size, generated-name, and exact
filters operate directly on match summaries. A single traversal of accepted
matches also counts functions retained on only one side of a filtered pair.

Whole-binary results are cached in memory by Binary Ninja file session and
BinaryView type. Pairs of clean, unmodified, fully analyzed views also use an
atomic persistent cache, keyed by file path, size, modification time, a SHA-256
digest of the file, BinaryView type, cache schema, Binary Ninja core version,
and A-to-B order. A raw binary (one not opened from a BNDB) is reanalyzed when
it is opened, so its key also includes a digest of the effective settings
outside the `ui.`, `network.`, `updates.`, `python.`, `pluginManager.`, and
`binjaDiffView.` groups; changing an analysis setting therefore misses the
cache instead of reusing results from different analysis. A BNDB's saved
analysis is covered by its content digest.

The digest is computed once per file session and reused until the file's path,
size, modification time, or change stamp changes. The change stamp is the
device, inode, and nanosecond ctime on macOS and Linux, and the volume serial,
file index, and NTFS change time on Windows; ordinary file APIs cannot restore
either after rewriting a file. Reversed A/B workspaces use a separate security
assessment. Concurrent requests for the same A-to-B orientation share one
worker; failed, cancelled, stale, malformed, or unsaved results are not
persisted.

The input extraction is cached independently for each clean, saved BinaryView
under the same identity rules. Its payload has a schema-specific path and a
SHA-256 integrity check. This lets an A-to-C comparison reuse the exact A
snapshot previously extracted for A-to-B. Any analysis, symbol, or byte update
drops the in-memory snapshot. The exception is the function update Binary Ninja
reports, on the requesting thread, when extraction itself requests a lazily
restored function's IL. Invalidating on it would discard that extraction and
restart it indefinitely, with or without unsaved edits in the database. Every persistent write, including the
background write after a save, hashes the file identity and writes a temporary
file, then commits it by atomic rename under the same mutex that invalidation
uses to delete artifacts. It commits only if the view epochs are unchanged and,
for a pair, no Rematch has invalidated it since the snapshot was taken. This
validity is tracked separately from memory retention, so a snapshot the memory
cache declines to keep (a zero budget or an oversized result) is still written.
An edit, save, or Rematch made meanwhile cancels the write: an older snapshot is never filed under a
newer saved identity, and a deleted pair artifact is not recreated; a modified/analysis-changed view cannot read or
write the persistent form. Saving publishes an already-current extraction under
the saved file's new identity, which includes its content digest.

The process extraction cache also retains each snapshot's prepared Rust handle.
It uses least-recently-used eviction with a 64-entry ceiling and a default
512 MiB estimated allocation budget, including extraction JSON and prepared
features. Set `BINJA_DIFF_SNAPSHOT_CACHE_MB` before launching Binary Ninja to
change the budget (0 disables retention; maximum 16384 MiB). Oversized snapshots
remain usable by the current comparison but are not retained. Preparation is
serialized per snapshot, and waiting comparisons can cancel.

These are retained-allocation estimates, not a process RSS limit. Active
comparisons and corpus results can hold additional memory; pair results have a
separate budget described below. Persistent disk artifacts are outside this
memory budget. Prepared handles are
process-local and do not change the persistent schema. Rust's `analysis_time`
measures matching after preparation; use outer elapsed time for end-to-end
performance comparisons.

Whole-pair results use a separate **256 MiB / 128-entry** retention limit. Set
`BINJA_DIFF_PAIR_CACHE_MB` before launch to change it (0 disables cache ownership;
maximum 16384 MiB). Accounting estimates vector/string allocations, map nodes,
and retained matching evidence. A non-owning index tracks the exact current
result after the LRU releases it, allowing open views and corpora to keep using
their existing shared object. Replacement and invalidation still make older
snapshots stale. Closed views release their own references; expired index entries
are pruned as the cache is used.

The scheduler, results panels, and match workers now pass and retain shared
snapshots directly. `matchFunctionsRef` returns a shared result even when it is
oversized or cache retention is disabled; the older value-returning API remains
available for callers that need an independent copy. Cold publication moves the
decoded result into shared ownership instead of cloning its complete tables.
These limits govern cache ownership, not the total memory retained by open
corpora, panels, overlays, active workers, or the weak-index metadata.

The persistent extraction and match caches share a default **4 GiB / 512-file**
retention limit under `binja_diff_view/extractions` and `binja_diff_view/matches`.
Set `BINJA_DIFF_DISK_CACHE_MB` before launch to change the byte budget (maximum
1048576 MiB). Zero bypasses disk reads and writes while leaving existing files
in place. Successful disk reads refresh the artifact's modification time for
retention ordering. Writes skip artifacts larger than the whole budget, and
reads bypass existing oversized artifacts.

After a successful write, cleanup removes oversized legacy artifacts first,
then the least recently accessed artifacts until both limits are met. It checks
only generated hash filenames in those two directories, excluding symlinks,
temporary files, manifests, and exports. Retention is best effort: pre-existing
files are trimmed on the next successful cache write, and concurrent processes,
temporary writes, or deletion failures can temporarily exceed the limit. Smaller
budgets trade disk space for recomputation. In-memory matches remain usable.

The Open Workspace action probes this cache against the existing analyzed views
before constructing new graph frames. The probe runs in the background behind a
progress dialog because it hashes both files on first use; the Open Corpus
Workspace action likewise hashes its binaries in the background before the
workspace opens. Cancelling either dialog leaves no workspace open. A hit populates the function table first,
then queues only the selected function's block/line rendering. Saving either
participating BNDB rewrites the completed pair under the file's new identity, so
the save itself cannot make the next workspace open look like a cold diff. That
rewrite, and the corpus identity refresh after a save, run in the background;
the save itself does not wait for them. A
cache miss runs one background whole-binary match; it does not run a second,
provisional extraction path.

A prepared cache hit records both file sessions, both BinaryView types, both
view epochs, and the exact shared cache object. The graph frames validate all of
them on the next event turn. If analysis invalidated or replaced the pair in the
meantime, an ordinary workspace uses the current cache or rematches. Corpus
drill-down refuses the open because substituting current results would mix two
snapshots.

### Concurrent cache misses

Cold whole-binary requests coordinate by directed pair, and extraction requests
coordinate by BinaryView identity. A waiting worker rechecks the cache and
captures the current revision after admission, so concurrent workspace opens
reuse a completed result where the cache retains it. Unrelated pairs can still
run independently. Waiting workers poll cancellation every 50 ms; an owner
failure or cancellation releases the gate for another attempt. Work gates hold
no global cache lock during extraction, Rust matching or file I/O and retain no
result payloads. Oversized or disabled caches can still require sequential
recomputation. This does not add parallel corpus scheduling.

### Cache-hit costs

On a cache hit, the whole-binary matcher does not run again (the persistent
artifact's timestamp and digest stay unchanged), and opening the workspace
requests no reanalysis. Any remaining delay comes from native widget and
ViewFrame setup and from rendering the selected function's overlay. The live
measurement behind this is recorded in [schema and validation
history](history.md#live-cache-hit-verification).

Analysis and symbol changes invalidate affected entries. **Rematch** explicitly
invalidates the active pair and starts matching again. Matching waits for two
consecutive idle analysis checks; if analysis changes during a run, the stale
result is discarded and retried. An extraction invalidated by the same event is
also treated as stale instead of being reported as user cancellation. Worker
exceptions are caught before they cross QtConcurrent, which preserves the
original error instead of the sliced message `std::exception`. Switching
presentation cancels stale deferred activation while retaining a completed
overlay for both Graph and Linear views.

| State | Lifetime |
|---|---|
| Function matches | process and persistent cache, until change/rematch |
| Per-binary function extraction | process and persistent cache, until view change |
| Manual pairs | process cache, until invalidation/rematch |
| Recent selected-function diffs | eight-entry process LRU, until view change/recompute |
| Active diff overlay | until Clear, recompute, or workspace close |
| Table search/filter/sort/selection | workspace lifetime |
| Corpus matrix and progress | corpus workspace lifetime; pair artifacts persist |
| Ported symbols | target BinaryView; persistent after saving the BNDB |

Manual overrides, types, comments, and reports are not currently persisted by
the plugin.

## Development deployment

Binary Ninja loads the installed native library from the top-level user plugin
directory, not from this repository's `build/` directory. When the repository
itself is the `binja_diff_view/` child of that plugin directory, CMake detects
the layout and copies each successful build to the live top-level plugin path,
along with the private Rust engine. This refresh is controlled by the
`BINJA_DIFF_REFRESH_LIVE_PLUGIN` CMake option, which is on by default. In a
build directory configured with it off, run `cmake --install build` after
building. This prevents a restart from silently
loading a stale installed library. Native code already mapped into a running
Binary Ninja process is unchanged, so Binary Ninja must still be restarted
after a build.

From this repository layout, `build/binja_diff_view.dylib` is only the build
artifact. Binary Ninja loads the sibling `../binja_diff_view.dylib`. A
successful live-refresh build makes their hashes match:

```sh
cmake --build build --target binja_diff_view
shasum -a 256 build/binja_diff_view.dylib ../binja_diff_view.dylib
```

Different hashes mean Binary Ninja will load stale native code after restart.

## Binary Ninja similarity API direction

Binary Ninja 6's
[experimental similarity branches](https://github.com/Vector35/binaryninja-api/branches/all?query=similarity)
introduce provider, resolver, directed session-graph, result-application, and
diff-rendering APIs.
The current upstream documentation also states that session graphs and result
lists remain in memory and are not stored in BNDBs. Consequently, those APIs do
not replace this plugin's durable prepared-pair artifact or its instant-reopen
lifecycle.

When the similarity API reaches the installed C++ SDK and supported editions,
the intended integration is to expose the Rust matcher as a similarity provider,
move acceptance policy into a resolver, and adapt the existing graph/linear
overlay to the similarity render context. The standalone backend remains
necessary for current Personal builds and as the persistence layer until Binary
Ninja similarity sessions themselves become durable.
