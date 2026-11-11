# Community catalog publication

Submit Binja Diff View as a **view-only native plugin**. Users install
the native package manually or build from source. Catalog maintainers must
enable `view_only` for the listing; `plugin.json` alone does not enable it.
There is no Python installer or automatic native-library download.

See Binary Ninja's [publishing documentation](https://docs.binary.ninja/dev/plugins.html)
and [metadata template](https://github.com/Vector35/sample_plugin/blob/master/plugin.json).

## Metadata

The root `plugin.json` records plugin version **0.1.6**. Its
`api` list is empty because this native plugin has no scripting provider or
Python dependencies. `platforms` advertises only `Darwin`, the platform with
a released prebuilt package. Source build instructions for other platforms
do not establish tested binary compatibility.

`minimumbinaryninjaversion` records the tested build number, `11108`, the
build the v0.1.6 package was compiled against. Source builds also worked on
`11052`, which used UI ABI 20.
Prebuilt libraries additionally require the core ABI, UI ABI, Qt version, and
CPU architecture recorded in their release notes. Update this information
when validating another Binary Ninja build or adding a supported platform.

`longdescription` is deliberately empty so the catalog displays `README.md`.
Keep README images and documentation links as full GitHub URLs. Keep the
metadata's MIT license text consistent with the root `LICENSE`, and retain
the bundled engine's separate copyright and license in distributed packages.

## Release and submit

1. Validate `plugin.json` using Vector 35's current
   [metadata checker](https://github.com/Vector35/community-plugins/blob/master/generate_plugininfo.py):

   ```sh
   python3 /path/to/community-plugins/generate_plugininfo.py -v plugin.json
   ```

   The README fallback warning is expected.
2. Review installation commands and compatibility details. For any new binary
   package, perform a clean installation and the focused GUI checks in
   [Testing](testing.md). Record the actual results in the release notes.
3. Commit the metadata and documentation, and publish a new GitHub release
   tagged `v<version>` at that commit. Keep earlier releases: `v0.1.3` is the
   last package for UI ABI 20. When attaching binaries, package both native
   libraries, installation instructions, build/ABI details, licenses, and
   checksums, and point the README's install section at that release.
4. File a [community plugin submission issue](https://github.com/Vector35/community-plugins/issues/new/choose)
   with the repository URL below. Request **view-only** publication explicitly.

   ```text
   Repo URL: https://github.com/meerkatone/binja_diff_view

   Please list Binja Diff View as view_only. This is a native C++ UI plugin
   with a bundled Rust engine, installed manually from GitHub releases or
   built from source. The available prebuilt package is for macOS ARM64 and
   Binary Ninja 6.1.11108-dev; its core/UI ABI and Qt requirements are in the
   README and release notes. plugin.json is at the repository root.
   ```

For later releases, increment `version` in `plugin.json` before tagging.
The catalog reads that field from the release's commit; the release title
and tag name alone do not update the metadata version.
