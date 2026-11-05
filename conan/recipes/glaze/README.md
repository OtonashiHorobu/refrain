# glaze

In-repo Conan recipe for glaze 9.1.0.

| Field | Value |
|---|---|
| Package | `glaze/9.1.0` |
| License | MIT |
| Upstream | <https://github.com/stephenberry/glaze> |

Header-only JSON and TOML library used for configuration documents.

Header-only; the recipe only exports the include tree.

## Building

```bash
python3 tools/vendor_export.py --build --only glaze
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
