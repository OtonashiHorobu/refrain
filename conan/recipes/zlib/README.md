# zlib

In-repo Conan recipe for zlib 1.3.1.

| Field | Value |
|---|---|
| Package | `zlib/1.3.1` |
| License | Zlib |
| Upstream | <https://github.com/zlib-ng/zlib-ng> |

zlib API provided by zlib-ng in compatibility mode.

Installs as zlib 1.3.1 so packages that depend on zlib resolve the same target.

## Building

```bash
python3 tools/vendor_export.py --build --only zlib
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
