# zlib

In-repo Conan recipe for zlib 1.3.1.

| Field | Value |
|---|---|
| Package | `zlib/1.3.1` |
| License | Zlib |
| Upstream | <https://github.com/zlib-ng/zlib-ng> |

zlib API provided by zlib-ng in compatibility mode.

Installs as zlib 1.3.1 so packages that depend on zlib resolve the same target.

## Patches

`patches/crc32-combine-negative-length.patch` backports the negative-length
guard for `crc32_combine()` and `crc32_combine_gen()` from zlib-ng commit
[`c6df39a06a4d6aa701b25bd4b6b146dde4156e02`](https://github.com/zlib-ng/zlib-ng/commit/c6df39a06a4d6aa701b25bd4b6b146dde4156e02).
Without it a negative length never terminates the signed loop inside
`x2nmodp()` (CVE-2026-27171). The recipe applies the patch in `source()` and
exports it with `exports_sources`; remove the patch when the pinned zlib-ng
release already contains the guard.

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
