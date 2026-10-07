# Schema 22 corrections and validation

This update fixes the defects found in the public-source review without
attempting to copy another tool's confidence percentages.

## What changed

- Security extraction preserves operand types. String and data-reference
  operands, quoted literals, and code-symbol references outside calls provide
  context rather than direct operation evidence. A string-only operand change
  no longer produces Strong 90%. Executed calls retain named-target evidence.
- The native MD-inspired key uses BFS depth from the actual entry instead of
  address-ordered DFS ranks. Sorting edge contributions before summation also
  removes floating-point dependence on block iteration order. The demonstrated
  block permutation and branch relocation now preserve the key.
- CFG fallback scoring combines count similarity with refined topology labels.
  Edge scoring compares endpoint-label pairs. Different connectivity with the
  same node/edge counts no longer automatically receives two perfect scores.
  These remain structural proxies, not proof of graph isomorphism.
- Tentative matching context is pruned when contradictions cancel its support
  or when it loses an anchored support path. Rejected pairs cannot re-enter
  refinement. If final assignment rejects a surviving context pair, that pair
  and unsupported dependents are removed and assignment repeats against the
  smaller context set. Retries terminate because context strictly decreases.
  This is explicitly a heuristic refinement, not a monotone lattice solver.
- Relevant includes Strong/Conflicting plus Possible with direct score >= 0.39,
  or Possible with direct score >= 0.13 and total score >= 0.30. The latter keeps
  changed operations with corroborating context visible after removing falsely
  direct string evidence. String-only context remains excluded. Strong is not
  broadened to compensate for weaker evidence.

The security fact lattice still uses componentwise max over levels 0/1/2 and
one-hop transfer from immutable direct facts. That implementation is unchanged.
Confidence remains an uncalibrated method score, not a probability of a correct
match or security fix. Older match artifacts are invalidated by schema 22;
extraction caches and BNDBs are retained.

## Focused validation

90 Rust tests pass; strict Clippy passes. Four regression tests were added for
the demonstrated defects: layout-independent MD/topology features, connectivity
scoring, typed operand evidence, and pruning invalid context with dependents.
The existing native model test now checks operation-plus-context inclusion and
string-only exclusion. The native plugin builds and that model test passes.
Licensed integration and full GUI testing were not repeated for these changes.

Saved input identities and raw replay responses remain local under
`build/schema22-replay/`. The comparison uses the pre-schema22 engine backup
against the corrected release engine on identical extraction payloads.

| Input | Schema21 matches | Schema22 matches | Counterpart changes among retained matches | BinDiff agreement before/after |
| --- | ---: | ---: | ---: | --- |
| screensharingd, current extraction | 1882 | 1882 | 0 | 1786 / 1786 |
| screensharingd, historical extraction | 1884 | 1884 | 0 | 1783 / 1783 |
| nwifi | 1005 | 1005 | 0 | 1003 / 1003 |
| tcpip | 5581 | 5581 | 0 | 5581 / 5581 |
| spoolsv | 2332 | 2332 | 0 | 2331 / 2331 |
| curl | 1410 | 1407 | 0 | No reference used |

The stricter scoring leaves these three old curl pairs unmatched:

- `_Curl_bufq_skip_and_shift` → `_Curl_h1_req_parse_free` (previous Heuristic).
- `_CONNECT_host` → `_Curl_http_proxy_get_destination` (previous CallGraph).
- `_new_unencoding_writer` → `_Curl_client_create_writer` (previous CallGraph).

Those losses follow the structural score correction; without independent
correspondence evidence, they should not be counted as three proven accuracy
improvements or restored merely to preserve the old total. Two shuffled input
orders retain every corrected screensharingd and curl pairing.

In the current screensharingd extraction, Strong falls from 15 to 3 and Relevant
from 15 to 10. HandleViewerAuthenticationMessages, SendRSAResponse, and
SendSRPChallenge are now Possible but remain Relevant; SaveAdvertisedAuthTypes
remains Strong. The nwifi candidate remains Possible and Relevant. The tcpip
candidate remains below Relevant and remains available through size filters.
Corrected Relevant counts are 21 for spoolsv and 40 for curl. These counts
measure review-queue membership, not validated vulnerability discoveries.

The 400-duplicate-function microbenchmark retains all 400 matches. Median time
was 107.9 ms for schema21 and 116.1 ms for schema22 in these runs; process peak
RSS was 156.2 MB and 156.5 MB. The added structural/context checks have a modest
cost in this synthetic case. These measurements do not establish corpus-wide
performance or accuracy.

## Public-source boundaries

[BinDiff scoring](https://github.com/google/bindiff/blob/677461658461af48810887570128803b834bf470/differ.cc#L490)
uses established graph correspondences, whereas this plugin uses cached
structural proxies during candidate scoring. Our MD key is also a native
variant, not numerically compatible with
[BinDiff's graph attributes](https://github.com/google/bindiff/blob/677461658461af48810887570128803b834bf470/graph_util.h#L63)
or [Diaphora's index](https://github.com/joxeankoret/diaphora/blob/621ec2699255fa709b2ee422cd919b38f088f16b/diaphora_ida.py#L2511).
Those implementations informed the review; they do not calibrate our scores.

Restart Binary Ninja to load the installed update. Previous libraries are
retained in `build/pre-schema22-20260905/`. No user database was modified.
