# libarchive

In-repo Conan recipe for libarchive 3.8.11.

| Field | Value |
|---|---|
| Package | `libarchive/3.8.11` |
| License | BSD-2-Clause |
| Upstream | <https://github.com/libarchive/libarchive> |

Archive reader used for package tar streams.

The recipe disables every external compression filter; decompression runs in the project pipeline.

## Building

```bash
python3 tools/vendor_export.py --build --only libarchive
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
