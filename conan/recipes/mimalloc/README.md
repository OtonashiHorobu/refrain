# mimalloc

In-repo Conan recipe for mimalloc 3.6.0.

| Field | Value |
|---|---|
| Package | `mimalloc/3.6.0` |
| License | MIT |
| Upstream | <https://github.com/microsoft/mimalloc> |

Allocator backend.

Static build used only when the allocator override is enabled.

## Building

```bash
python3 tools/vendor_export.py --build --only mimalloc
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
