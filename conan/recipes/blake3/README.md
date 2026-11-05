# blake3

In-repo Conan recipe for blake3 1.8.7.

| Field | Value |
|---|---|
| Package | `blake3/1.8.7` |
| License | CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception |
| Upstream | <https://github.com/BLAKE3-team/BLAKE3> |

BLAKE3 hash function.

Built with the portable C implementation plus the project build flags.

## Building

```bash
python3 tools/vendor_export.py --build --only blake3
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
