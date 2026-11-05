# benchmark

In-repo Conan recipe for benchmark 1.10.0.

| Field | Value |
|---|---|
| Package | `benchmark/1.10.0` |
| License | Apache-2.0 |
| Upstream | <https://github.com/google/benchmark> |

Google Benchmark microbenchmark library.

The ConanCenter recipe pins an older release; this recipe builds the upstream tag the project benchmarks against.

## Building

```bash
python3 tools/vendor_export.py --build --only benchmark
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
