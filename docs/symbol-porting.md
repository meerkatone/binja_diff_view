# Symbol porting and patch-diff testing

Binja Diff View can copy function symbols from either side of an accepted or
explicitly chosen pair. It does not copy types, variables, or comments.

## Port matched symbols

1. Select one or more **Matched** or **Changed** rows.
2. Right-click and choose **Port selected symbols A → B…** or **B → A…**.
3. Review the source, target, skipped names, and conflicts.
4. Apply the plan.

Bulk visible-row actions use `binjaDiffView.symbolPortMinConfidence` (default
`0.9`). Explicitly selected rows are not confidence-filtered.

## Pair unmatched functions

1. Select one A-only row.
2. Command-click (Control-click on Linux/Windows) one B-only row.
3. Choose the unmatched-pair port action in the required direction.
4. Verify both addresses and apply the rename.

The pair becomes an in-memory manual match and can immediately drive graph
diffing. A later full rematch currently discards manual pairs.

For functions reached outside the table, use **Port current graph symbol…**.
The action resolves the functions at the two displayed cursor addresses.

## Safeguards and persistence

- Generated source names are rejected because they provide no useful symbol.
- Generated target names such as `sub_...`, `func_...`, `nullsub_...`, and
  `j_...` may be replaced.
- Meaningful target names are preserved unless overwrite is explicitly approved.
- A plan cannot assign multiple source names to the same target.
- Target names are rechecked after preview to avoid overwriting concurrent edits.
- Each batch is one Binary Ninja undo operation.
- The cache refresh replaces only the exact pair used to build the plan. A
  concurrent function or byte change invalidates that pair and forces a rematch.
- Save the target BNDB to persist applied symbols.

Similarity is evidence, not proof. Review weak or security-sensitive pairs
before transferring names.

## Patch-diff fixture

The tests build:

- `memcpy_safe_fixture`, which retains `copy_record` and checks the destination
  length before `memcpy`;
- `memcpy_bug_fixture.stripped`, which omits that guard and strips local symbols;
- the same target before stripping, which supplies known function addresses.

Both variants also contain an unchanged local `symbol_port_fixture` helper for
automatic matching and symbol-transfer validation. The target has no debug
information that could restore stripped names.

Use the separate Commercial headless build described in [Testing](testing.md):

```sh
cmake --build build-commercial-headless --target binja_diff_symbol_patch_integration_tests
ctest --test-dir build-commercial-headless -L integration --output-on-failure
```

The fixture test verifies the symbol difference. Commercial headless integration
matches and ports the unchanged helper, checks undo/redo and BNDB persistence,
and compares the changed wrapper's MLIL. Signature setup uses an explicit Manual
fixture correspondence when the changed wrapper is unmatched automatically;
that case is reported in the log. Reference-string evidence is checked
independently of matching. Personal is used for GUI validation, not headless.

## Troubleshooting

### Port actions are missing

Restart Binary Ninja after installing a new build. Existing workspaces retain
the plugin code loaded when they were created.

### Nothing to port

Check that:

- a matched row or exactly one A-only/B-only pair is selected;
- the source has a meaningful name;
- the target does not already have the same name;
- a meaningful target name is not protected;
- a bulk-visible row meets the confidence floor.

### The Rust engine fails to load

Reinstall the plugin. An installed plugin loads the engine only from its private
directory (or from `BINJA_DIFF_ENGINE_PATH`); an old top-level `librust_diff`
copy is ignored and can be removed. The correct layout is:

```text
plugins/binja_diff_view.dylib
plugins/.binja_diff_view/librust_diff.dylib
```

### The plugin fails after a Binary Ninja update

Run the normal build again. CMake watches the macOS application bundle version
and automatically fetches the exact matching `dev/<version>` or
`stable/<version>` API tag before rebuilding. If Binary Ninja also changed its
bundled Qt minor version, install that matching Qt SDK and configure a fresh
build directory. A nearby API tag or different Qt build is not ABI-compatible
for this native plugin.
