# Schema and validation history

This file preserves earlier schema notes and validation measurements that were
previously part of [How Binja Diff View works](how-it-works.md). Entries
describe the behavior and results at the time they were recorded; later schemas
supersede parts of them. For current behavior, see the main document. Match
schema 25 is current. Changes in schemas 23 and 24 were not recorded separately
here.

Some measurements rely on private inputs that are not in this repository, such
as analyst-labelled BNDBs and `strings_delta.csv`. They are recorded as
evidence and cannot be reproduced from the repository alone.

## Security triage by schema

- schema 15 scores every changed or added function in a separate finite product
  lattice. Direct patch-delta facts join with one-hop call-neighbor context to a
  closure. Schema 20 computes that closure in one pass over immutable direct
  facts. Only direct level-2 facts transfer, propagated level-1 facts do
  not retransmit, and a target must contain a direct patch mechanic before it
  accepts context. A `Strong` result additionally requires direct security-
  semantic evidence, preventing a branch-only relocation from inheriting the
  label of a distant authentication function. Schema 21 observes both added and
  removed normalized instructions, with direction recorded in evidence labels.
  Schema 22 also masks StringToken, DataSymbolToken, and non-call CodeSymbolToken
  operands when extracting direct operation deltas. Their text supplies level-1
  context, including when it is absent from the separate strings list. A string-only change cannot
  produce Strong; ambiguous non-binding pairs are capped at Possible. Conditional
  x86 jumps and ARM condition branches count as guards; ARM bitwise operations
  and unconditional branches do not. Size-direction filters are independent of
  security scores, so low-scoring structural changes remain accessible.
- schema 16 stores the baseline and patched identities in requested order.
  Reversed workspaces use a distinct pair artifact, so a cached security score
  cannot be interpreted against the wrong patch direction. Extraction schema 11
  resolves function-to-string evidence through Binary Ninja code references,
  one level of data wrapping such as CFString objects, and derived-string xrefs
  when the installed API provides them. Renderer tokens remain a fallback.
  Derived-string enrichment fails safely when the core cannot materialize a
  record, and JSON encoding replaces malformed UTF-8 from analysis metadata.
- schema 17 puts `Conflicting` rows back into the Relevant review filter and
  ignores generated `WPP_<hex>_Traceguids` operands when looking for
  maintenance-only vocabulary. It rejects schema-16 pair artifacts so the
  corrected assessment is recomputed from the cached extraction.
- schema 18 requires two independent direct patch mechanics before joining
  one-hop context. Relevant keeps every Strong and Conflicting row, but admits a
  Possible row under its original rule only when its direct score is at least
  0.39. Schema 22 retains that path and also admits Possible rows with direct
  score >= 0.13 and total score >= 0.30. This keeps operation-plus-context
  candidates visible after correcting falsely direct string evidence. Pure
  context (direct score zero) remains excluded. Strong remains narrower.
- schema 19 left support-versus-contradiction ties in the candidate `may` set
  instead of promoting them to fixed-point anchors. Schema 20 replaces that
  promotion mechanism as described above. Patch-signature schema 9
  records whole-block insertions and deletions as semantic lines, treats a
  confirmed absent anchor block as evaluated zero-similarity evidence, prefers
  HLIL over compiler-oriented MLIL, and retains line addresses for navigation.
  It scores patch blocks by diff indices so structured nodes may safely share a
  source address. Graph-local labels and generated jump-table relocations are
  cosmetic; changed bounds and call targets remain semantic.
- schema 25 / extraction schema 12 fixes false `Strong` rows in a GUI test of
  `screensharingd` 26.6 (25G72) → 26.6.1 (25G76). `sub_100002824` and similar
  functions had equal sizes and zero instruction delta but were `Changed ·
  Strong 92.0%`. Four causes contributed:
  - Lazy function restore. Binary Ninja restores database functions without
    their analysis until IL is requested. The 26.6.1 extraction, taken from a
    freshly opened BNDB, held 87 import and 55 code-symbol call operands against
    3,890 and 4,050 for 26.6, whose functions had been analyzed. Almost every
    call therefore differed (`bl 0x1000749b8` against `bl sub_100074700`). A
    re-extraction after reopening reproduced it, so this was not a stale cache.
    Extraction now requests LLIL for every function first.
  - Build-root churn. Every `__FILE__` assertion path differed only in
    `BuildRoots/<random>/` and `TemporaryDirectory.<XXXXXX>/`, and the unchanged
    remainder supplied identity, protocol-state, and remote-input context. Path
    strings are now compared with generated components masked.
  - Page relocation (AArch64). `add x3, x3, #0x97` against `#0x487` for the
    same relocated string counted as an instruction change. The `adrp` before it
    rendered the symbol or string that happened to start on its page
    (`method_copyWithZone:.types` against an Objective-C stub name), and that
    page string also entered the function's string set. Page offsets that form
    a resolved image address are now addresses, the `adrp` operand is one page
    address, and string context comes only from each non-`adrp` instruction's
    own references.
  - Header strings. Small offsets such as `#0x1c0` were also tried as
    image-relative addresses, landing in the Mach-O header and adding
    `/usr/lib/dyld`, `__const`, and framework paths to many functions. That
    fallback now requires a section.

  On the same pair, schema 24 reported 715 changed, 349 cosmetic, and 820 exact
  matches with 41 `Strong` and 123 `Security: relevant` rows. Schema 25 reports
  48 changed, 945 cosmetic, and 891 exact, with no `Strong` row and two relevant
  rows: `HandleViewerAuthenticationMessages` and `SendRSAResponse`, both
  `Possible` with an added guard and authentication string context.
  `sub_100002824` went from 44 changed, 1 removed, and 1 added blocks to 2
  changed. Most remaining small deltas are `__LINE__` arguments and `malloc`
  type identifiers, which stay semantic. During development a page-offset check
  that accepted image-base-relative addresses made ten such functions cosmetic,
  and a generic path-token mask hid names like `AuthV1Policy`; both were
  narrowed before release. The memcpy accuracy fixture is unchanged: four
  correct, zero wrong, 80% recall, symmetric.

The `Strong` classification rule is unchanged from schema 15. The schema-16
`Security: relevant` UI filter is narrower than its predecessor: it selects
only `Strong` and `Possible`, rather than every row whose raw score is at least
0.30, and therefore excludes `Conflicting`. Richer extraction-schema-11 string
and xref evidence can still promote an individual row out of `Unknown`; that is
an evidence change, not a broader filter. Security labels remain heuristic
triage rather than vulnerability findings.
Schema 17 fixes the resulting recall regression. Relevant now includes
`Strong`, `Possible`, and `Conflicting`; Strong remains the narrower direct-
evidence view.
Schema 18 narrows the Possible part of Relevant to three direct evidence
dimensions. This prevents a lone new comparison plus inherited neighbor context
from filling the review queue.

## Comparisons and replays

The schema-12 baseline includes three BinDiff 8 comparisons made from the same
binaries:

| Pair | BinDiff overall | Native overall | Same accepted address mappings |
|---|---:|---:|---:|
| `screensharingd` 26.6 to 26.6.1 | 98.5144% | 99.0515% | 1,741 / 1,878 |
| `nwifi.sys` 18874 to 20680 | 99.0298% | 99.6767% | 1,003 / 1,004 |
| CVE-2024-38063 vulnerable to patched | 99.2718% | 99.9617% | 5,581 / 5,581 |

Every named mapping agrees. The 137 `screensharingd` disagreements and the one
`nwifi.sys` disagreement are address-generated functions, where repeated small
helpers make one-to-one assignment ambiguous. These results compare behavior;
they do not establish either matcher as ground truth.

Replaying the same extraction payloads with schema 13 preserved match counts,
unmatched sets, content states, and similarity scores. For `screensharingd`, 37
Native assignments changed. Agreement with BinDiff rose from 1,741 to 1,758 of
the 1,878 shared mappings: 17 mappings moved into agreement, none moved out of
agreement, and 19 changed to a third assignment. Matched call-edge consistency
rose from 6,424 to 6,444 edges. The `nwifi.sys` and CVE-2024-38063 mappings were
unchanged. BinDiff is comparison evidence rather than ground truth, so the
remaining generated-function ambiguity still needs independent labels.

Schema 14 replayed the live `screensharingd` extraction without changing match
count, unmatched sets, content states, or similarity. It changed 52 generated
assignments relative to schema 13. Agreement with the same BinDiff reference
rose from 1,758 to 1,779 of 1,878 shared mappings: 21 moved into agreement and
none moved out. Consistent mapped call edges rose from 6,446 to 6,476 of 6,519.
The matcher took 0.334 seconds in the measured release replay, compared with
0.308 seconds for schema 13. BinDiff remains comparison evidence rather than
ground truth.

Schema 15 preserves those schema-14 mappings and body scores; security triage
is a post-match projection. A controlled `screensharingd` 26.6 to 26.6.1 replay
started with 700 changed matches and four additions. Joining the three string
deltas recorded in `strings_delta.csv` reduced that set to three `Strong` rows
and seven `Strong`/`Possible` rows. The three direct rows were
`HandleViewerAuthenticationMessages` (which contains the analyst-labelled
`HandleAuthTypeMessage` region), `SendRSAResponse`, and the added
`SaveAdvertisedAuthTypes`; the changed `SendSRPChallenge` row is seventh through
one-hop context. This controlled replay used the generated `sub_<address>`
symbols that were present at measurement time, so its result was not caused by
analyst names. The BNDBs were subsequently updated and independently reloaded
on 2026-09-01; the four role names above were verified at their exact function
starts in both versions. The replay enriched an older extraction with the
observed strings. A fresh schema-10 UI run remains the end-to-end extractor
check.

The rematched open pair did not reproduce that controlled count. Its 712 changed
matches and four additions produced seven `Strong` and thirteen
`Strong`/`Possible` rows. All four known bug functions remain in the Strong
queue, so the live filter reduces 716 rows to 13 (98.2%) without losing one of
those functions. Three other changed functions score Strong and six score
Possible. The security pass assumes that side B is the patched side. Schema 16
records that contract as `security_baseline_identity` and
`security_patched_identity` and keeps reversed artifacts separate. The 3/7
controlled result is not the expected UI count. Those measurements predate the
schema-16 cache and extraction changes and require a rematch before comparison.

The subsequent live schema-16 `spoolsv` rematch records vulnerable build 2336
as A and fixed build 2525 as B. It produced 2,332 accepted matches, 21 Changed
rows, no baseline-only functions, 16 patched-only functions, 99.0677% overall
similarity, and 99.3186% coverage. BinDiff accepts the same number of functions
and agrees on 2,331 address mappings (99.9571%). The sole disagreement remains
`YAsyncGetRemoteNotifications`: BinDiff selects `AcquireChannel`, while Native
selects `AsyncGetNotifications` through `CallGraph` evidence.

That run has zero `Strong`, 22 `Possible`, and two `Conflicting` rows across
the 21 changed and 16 patched-only functions. The schema-16 `Relevant` filter
showed 22 rows. Schema 17 restores the two Conflicting rows, so the same
assessment produces a 24-row review queue after rematching.

The schema-16 filter change reduced recall on the `nwifi.sys` fixture. Its
schema-15 and schema-16 artifacts have identical mappings and identical
security assessments: zero `Strong`, one `Possible`, and one `Conflicting`.
The known `Dot11Translate80211ToEthernetNdisPacket` change is the Conflicting
row at 0.445. It surfaced under the old numeric Relevant predicate and is hidden
by the schema-16 state predicate. Its maintenance counter-signal is generated
by a changed WPP `Traceguids` symbol rather than proof that the patch is merely
logging work.

Schema 17 restores that row to Relevant even if another counter-signal leaves
it `Conflicting`. It also recognizes generated WPP trace-GUID identifiers as
instrumentation identity churn, so that token alone no longer supplies the
maintenance veto. Focused tests preserve both behaviors while retaining
maintenance evidence for ordinary tracing changes. A live restart and rematch
confirmed the known `nwifi.sys` function as `Possible` at 0.445 with no
maintenance counter-evidence; all 1,005 accepted mappings remained unchanged.

The schema-18 curl 8.3.0 to 8.4.0 replay preserves all 1,416 mappings while
reducing the Relevant queue from 50 rows to 17. The same replay gate retains the
known `nwifi.sys` function and all four known `screensharingd` functions.
The live UI rematch then reproduced 1,416 matches, 28 A-only rows, and 43 B-only
rows. Weak rows remained in All functions with no security label, confirming
that schema 18 changed triage without changing pair assignment.

## Live cache-hit verification

The persistent lifecycle was exercised against two clean PE BNDBs using both
Binary Ninja UI MCP and the Sidekick Bridge. The first workspace open produced
one 2.1 MB artifact containing 5,266 accepted matches. Opening another workspace
for the unchanged pair produced a second diff tab while the artifact's timestamp
and SHA-256 remained unchanged. Before and after both opens, both backends
reported `IdleState`, zero active analysis time, and no file or analysis
modifications.

Those observations distinguish the relevant costs:

- an unchanged artifact proves the whole-binary matcher did not run again;
- idle, unmodified BinaryViews prove workspace opening did not request BNDB
  reanalysis;
- any residual delay after a cache hit belongs to native widget/ViewFrame setup
  and rendering the selected function overlay.
