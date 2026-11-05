# wasmtime

In-repo Conan recipe for wasmtime 50.0.1.

| Field | Value |
|---|---|
| Package | `wasmtime/50.0.1` |
| License | Apache-2.0 WITH LLVM-exception |
| Upstream | <https://github.com/bytecodealliance/wasmtime> |

WebAssembly runtime for the script module.

The C API is linked statically; the recipe exports only the headers and the static library.

## Building

```bash
python3 tools/vendor_export.py --build --only wasmtime
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
