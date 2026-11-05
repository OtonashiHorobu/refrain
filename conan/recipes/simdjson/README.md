# simdjson

In-repo Conan recipe for simdjson 5.1.0.

| Field | Value |
|---|---|
| Package | `simdjson/5.1.0` |
| License | Apache-2.0 OR MIT |
| Upstream | <https://github.com/simdjson/simdjson> |

JSON parser.

Built with the runtime dispatch backend; no exceptions.

## Building

```bash
python3 tools/vendor_export.py --build --only simdjson
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
