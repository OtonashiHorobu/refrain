# crashpad

In-repo Conan recipe for crashpad 2026.10.26.

| Field | Value |
|---|---|
| Package | `crashpad/2026.10.26` |
| License | Apache-2.0 |
| Upstream | <https://github.com/getsentry/crashpad> |

Crashpad crash-capture client and handler, from the getsentry fork with CMake support.

The recipe builds the client library and the `crashpad_handler` executable for
Linux and for Windows with MSVC. A MinGW cross build from Linux cannot compile
the fork: its Windows sources include case-sensitive system headers such as
`<NTSecAPI.h>`, which only resolve on a case-insensitive filesystem. The server
has a native minidump fallback for builds without Crashpad, so the MinGW
package still records crashes.

The recipe fetches the fork and its submodule archives at pinned commits; the version is the upstream snapshot date.

## Building

```bash
python3 tools/vendor_export.py --build --only crashpad
```

The recipe downloads the pinned source archives and verifies their sha256
digests declared in `conanfile.py`. The dependency lock stores the package
revision, so a recipe change requires regenerating
`conan/locks/vendor.lock`.

When updating, change the version, the archive URLs and their sha256 digests
together, then rebuild the package and run the test suite.
