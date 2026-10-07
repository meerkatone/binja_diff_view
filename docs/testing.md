# Choosing checks

Keep routine validation small. The Rust regression suite is fast; saved-binary
replays, licensed integration checks, and benchmarks are separate opt-in steps.
No unit tests have been removed.

| Change | Checks |
| --- | --- |
| Rust matching or scoring | Rust tests; replay relevant saved pairs when counterpart selection or security classification changes |
| Native model or persistence | Native unit tests |
| Extraction, graph/UI plumbing, or symbol transfer | Native build, licensed integration test, and a focused GUI check |
| Performance-sensitive implementation | Relevant benchmark after correctness checks |
| Documentation only | Review links and commands; no binary replay |

## Routine checks

For Rust changes:

```sh
cargo test --locked --manifest-path rust_engine/Cargo.toml --lib
```

For native unit checks, use an already configured build directory:

```sh
cmake --build build --target \
  binja_diff_model_tests binja_diff_corpus_model_tests \
  binja_diff_corpus_persistence_tests binja_diff_corpus_metrics_tests \
  binja_diff_weighted_cache_tests binja_diff_disk_cache_tests \
  binja_diff_shared_result_cache_tests binja_diff_keyed_work_gate_tests \
  binja_diff_token_emphasis_tests
ctest --test-dir build -L unit --output-on-failure
```

This CTest selection includes the fast Rust suite. Running plain `ctest` also
runs only unit/fixture targets by default. The licensed integration
test is neither built nor registered unless explicitly enabled. Historical
corpus and curl headless utilities are excluded from the default build and
remain available as explicit build targets.

The disk-cache test uses a temporary directory to check combined byte/count
limits, access promotion, oversized legacy entries, and exclusions for exports,
pending files, and symlinks. It never trims the user's actual cache.

The work-gate test checks same-key exclusion, progress on other keys, cancellation
while an owner remains active, exception recovery, lease ownership and bounded
tracking metadata. Eight concurrent cache requests must perform their work once
and return the same object. Commercial integration separately exercises four
simultaneous requests through the real whole-binary matching path.

The corpus-metric tests check one-sided filtering, unmatched denominators, exact
and generated-name exclusions, and symbol-flag capture counts. A ten-build,
45-pair filter application captures flags ten times when needed and zero times
for size/name-only filters. A new application must observe fresh flags.

The shared-result cache test checks exact-object discovery after eviction,
replacement/invalidation, payload release after readers close, oversized and
disabled retention, and ownership across an asynchronous worker handoff. It
does not establish GUI behavior; reference-lifetime changes also need a focused
desktop check with corpus drill-down and more than one open result panel.

The diff-model tests check forward and reverse counterpart lookup when addresses
overlap across binaries. Corpus-model tests check that raw function/instruction
totals follow binary IDs and pair direction, and that repeated publications or
multiple edges replace totals rather than accumulate them.

For selected-function rendering or patch-signature changes, build the relevant
native tests and run only those checks:

```sh
cmake --build build --target binja_diff_token_emphasis_tests binja_diff_corpus_persistence_tests
ctest --test-dir build -R '^binja_diff_(token_emphasis|corpus_persistence)_tests$' --output-on-failure
```

Token-emphasis tests cover repeated operands, visible and hidden address
columns, the extracted empty separator, linear indentation, comments, and span
text mismatches. Persistence tests reject outdated patch signatures and clear
their classifications. Rendering changes also require the desktop checks below.

## Focused diff and corpus GUI checks

After installing the native plugin, restart Binary Ninja or open a fresh
instance to load it. Use existing analyzed BNDBs and leave them unsaved.

1. Open a pair workspace with an older binary as A and a newer binary as B.
   Select a function with removed and added instructions. Click **Diff** in B,
   navigate through both changes, then click **Diff** in A. A must remain red
   for removals and B green for additions, with the same function identities
   and scores throughout.
   Switch Graph → Linear → Graph with visible addresses; changed operands must
   keep their inline markers, and graph entry blocks must retain their colors
   even when a function-label line precedes the instructions. Click **Clear**:
   both panes must lose diff colors and markers. Open an unchanged function:
   it must show zero changed, removed, and added blocks without diff overlays.
2. Open an ordered-lineage corpus with three builds. Select a completed matrix
   cell and record its accepted-match count and mean similarity. Toggle
   **Score filters → Include exact matches** off and on. The selected cell's
   footer must update on each toggle without selecting another cell.
3. Export JSON. For each successful pair, compare its `raw_metrics` source and
   target function/instruction counts with the corresponding `binaries`
   records, using IDs. A build present in both adjacent pairs must retain its
   full count once. Confirm the expected pair/family counts and `partial` state.

Record input names, function addresses, screenshots, and the unmodified export
under `build/`. For an all-pairs check, compare five analyzed builds and verify
that all ten pairs complete before checking the export.

## Extraction and symbol integration

The committed memcpy extraction pair has five address-labelled counterparts,
including the changed `copy_record` wrapper. Run its accuracy check against a
release engine:

```sh
python3 tests/accuracy_memcpy_fixture.py build/rust-engine-target/release/librust_diff.dylib
```

The schema-24 baseline is four correct pairs, zero wrong pairs, 80% recall and
the same mapping in both directions. The changed wrapper remains unmatched;
its primary body score is 0.158, below the current call-neighborhood gate.
It now has a review hint for the stripped counterpart. The hint must remain
outside accepted matches and disappear when a second counterpart competes or
the candidate body is unrelated.
This small fixture measures one known miss and guards against regressions. It
cannot calibrate confidence or establish broad-corpus precision and recall.

Use a separate build against Binary Ninja Commercial and its matching API.
Personal does not support headless execution; use it for desktop validation.

```sh
cmake -S . -B build-commercial-headless \
  -DBINJA_DIFF_BUILD_UI_PLUGIN=OFF \
  -DBINJA_DIFF_RUN_INTEGRATION_TESTS=ON \
  -DBN_INSTALL_DIR="/path/to/Binary Ninja Commercial" \
  -DBN_API_PATH="/path/to/matching/binaryninja-api"
cmake --build build-commercial-headless --target binja_diff_symbol_patch_integration_tests
ctest --test-dir build-commercial-headless -L integration --output-on-failure
```

This builds the required memcpy fixtures and runs their symbol check plus the
Binary Ninja integration test. An unchanged local helper supplies the automatic
matching and symbol-transfer case. The changed wrapper's reference strings are
checked in either matched or unmatched results, including added/removed context
labels. Its selected-function comparison uses the fixture's unstripped symbols
as independent address ground truth. If the matcher abstains, signature setup
publishes an explicit Manual fixture correspondence and reports that fact.
This does not establish automatic discovery of the changed wrapper.

The target fixture is compiled without debug information and stripped, so debug
imports cannot restore the names being tested. The executable takes the
reference, stripped target, unstripped target, and output BNDB as its four
arguments. A missing Commercial headless license reports a skip; that is not a
successful integration validation. Use a focused GUI check for rendering changes.

Return to the default selection afterward:

```sh
cmake -S . -B build-commercial-headless -DBINJA_DIFF_RUN_INTEGRATION_TESTS=OFF
```

## Saved-input replays and performance

For corpus serialization changes, an opt-in benchmark exports synthetic family
records without opening BinaryViews:

```sh
cmake --build build --target binja_diff_export_benchmark
/usr/bin/time -l build/binja_diff_export_benchmark json build/export-benchmark.json 20000
/usr/bin/time -l build/binja_diff_export_benchmark csv build/export-benchmark.csv 20000
```

The `time -l` option measures peak process memory on macOS. Run each variant in
a fresh process when comparing memory. This benchmark is excluded from the
default build and CTest. The persistence test covers cancellation during output,
destination preservation, temporary-file cleanup, and publication errors.

For prepared-snapshot changes, save the previous release library before building
and compare it with the new release:

```sh
python3 tests/benchmark_snapshots.py /path/to/previous/librust_diff.dylib build/rust-engine-target/release/librust_diff.dylib > build/benchmark-snapshots.json
```

This synthetic ten-build corpus checks output parity across JSON and prepared
paths, including reversed extraction order and unmatched functions. Its prepared
timings include preparing all ten snapshots. It does not measure extraction,
GUI latency, or peak RSS. Unit tests separately cover cancellation, concurrent
snapshot readers, LRU eviction, and active-reader ownership after eviction.

For neighbor-refinement cost, run the sparse 1,000-function call-chain case in
separate processes against the previous and new libraries. Compare the mapping
digests as well as the median times:

```sh
python3 tests/benchmark_neighbor_refinement.py /path/to/previous/librust_diff.dylib > build/neighbor-refinement-before.json
python3 tests/benchmark_neighbor_refinement.py /path/to/new/librust_diff.dylib > build/neighbor-refinement-after.json
```

For selected-function block pruning, run the equal-block and unrelated-block
cases against both libraries. Both result digests must match across versions:

```sh
python3 tests/benchmark_block_pruning.py /path/to/previous/librust_diff.dylib > build/block-pruning-before.jsonl
python3 tests/benchmark_block_pruning.py /path/to/new/librust_diff.dylib > build/block-pruning-after.jsonl
```

Store generated replay responses, detailed summaries, and benchmark runs under
`build/`. They remain local and ignored by Git.
