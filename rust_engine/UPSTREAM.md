# Bundled Rust engine provenance

This directory vendors the Rust matching engine formerly consumed from the
separate `rust_diff` Binary Ninja plugin.

- Upstream: `https://github.com/meerkatone/rust_diff`
- Imported commit: `a0e15fed66b8a6635d68e475283cf34173c0731c`
- License: Apache-2.0; see `LICENSE`

Only the Rust crate needed to build `librust_diff` is included. The Python
frontend, screenshots, examples, and standalone plugin metadata are omitted.

The bundled crate now contains plugin-specific matching, scoring, IL
canonicalization, and FFI changes. Its scores are native to Binja Diff View and
are not compatibility outputs for the original `rust_diff`, BinDiff, or
Diaphora. See [the algorithm audit](../docs/accuracy-performance-fix-plan.md)
for the current differences and versioning policy.
