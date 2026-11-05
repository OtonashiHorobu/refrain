# steam-audio

In-repo Conan recipe for steam-audio 4.8.1.

| Field | Value |
|---|---|
| Package | `steam-audio/4.8.1` |
| License | Apache-2.0 |
| Upstream | <https://github.com/ValveSoftware/steam-audio> |

Spatial audio library for the client stack.

The recipe packages the prebuilt SDK archives published upstream.

## Building

```bash
python3 tools/vendor_export.py --build --only steam-audio
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
