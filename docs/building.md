# Building Refrain

## Prerequisites

| Component | Version | Notes |
|---|---|---|
| Linux x86_64 | kernel 6.x | the supported development platform |
| GCC | 16 or newer | C++26 is required |
| CMake | 4.2 or newer | presets are used |
| Conan | 2.31 or newer | dependencies and the lockfile |
| Ninja | any recent version | generator used by the presets |
| Python | 3.11 or newer | tooling and Conan |

Linux x86_64 is the supported development platform. MinGW-w64 cross builds and
native MSVC builds are compiled and tested in CI and produce the Windows
archives, but they are not the primary development environment. macOS is not
supported.

## Bootstrap

```bash
scripts/bootstrap.sh
```

The script performs these steps:

1. verifies the toolchain versions;
2. writes the `gcc-release` Conan profile if it does not exist;
3. builds the in-repo recipes into the local Conan cache
   (`tools/vendor_export.py --build`);
4. installs dependencies with `conan/locks/vendor.lock`;
5. configures and builds the `conan-release` preset;
6. runs the test suite.

Options:

| Flag | Effect |
|---|---|
| `--profile-only` | write the Conan profile and stop |
| `--offline` | reuse the warmed Conan cache, no downloads |
| `--skip-vendor` | skip the in-repo recipe build |
| `--no-tests` | build without running tests |

`REFRAIN_CONAN_PROFILE` overrides the profile name and `JOBS` overrides the
build parallelism.

## Manual build

```bash
python3 tools/vendor_export.py --build
conan install . -pr gcc-release --build=missing --lockfile=conan/locks/vendor.lock
cmake --preset conan-release
cmake --build --preset conan-release
```

Binaries are written to `build/Release/bin/`. The build directory is
`build/Release/` for the `conan-release` preset; other configurations require a
matching Conan preset.

## Build options

`cmake/refrain_options.cmake` defines the project options. The defaults enable
tests, benchmarks, crash reporting, reflection and mimalloc. Turn optional
features off to shorten a build:

```bash
cmake --preset conan-release -DREFRAIN_WITH_CRASHPAD=OFF -DREFRAIN_BUILD_BENCH=OFF
```

`REFRAIN_BUILD_BENCH_VARIANTS` builds the optional library-comparison benchmark
targets and stays off unless those sources are present.

## Hardening and optimization

Optimized builds use `-O3 -DNDEBUG`. Link-time optimization is on for those
builds (`Release`, `RelWithDebInfo`, `MinSizeRel`) and off for `Debug`,
sanitizer builds and MSVC, where it only costs link time. Hardening stays
opt-in; release archives use the hardened profile described below.

| Option | Default | Effect |
|---|---|---|
| `REFRAIN_HARDEN` | OFF | Stack protector and stack-clash protection, CET (`-fcf-protection=full`) on x86, hidden symbol visibility, `_FORTIFY_SOURCE=3`, and ELF `RELRO`/`BIND_NOW`/non-executable stack. MSVC uses `/guard:cf`; MinGW adds `--dynamicbase --nxcompat --high-entropy-va`. |
| `REFRAIN_HARDEN_MAX` | OFF | Adds `-ftrivial-auto-var-init=zero` and `-fzero-call-used-regs=used-gpr` on top of the baseline. |
| `REFRAIN_LTO` | ON | Link-time optimization for optimized GCC/Clang builds (the Conan `lto` option feeds it). `Debug`, sanitizer builds, MSVC and the MinGW cross build skip LTO automatically. |

In a regular build only first-party targets get these flags; third-party
libraries come prebuilt from Conan. The flags apply to `Release`,
`RelWithDebInfo` and `MinSizeRel`; `Debug` keeps the stack protector but skips
FORTIFY and LTO.

LTO dominates the link step of a `Release` build. For a faster edit-build-test
loop, turn it off for the local build only:

```bash
conan install . -pr gcc-release --build=missing -o "&:lto=False" \
    --lockfile=conan/locks/vendor.lock
cmake --preset conan-release -DREFRAIN_LTO=OFF
```

`scripts/dist.sh` is the release path. It rebuilds every Conan dependency from
source with the hardening flags in an isolated Conan home, builds the server and
tools with hardening enabled (and LTO on Linux), strips the binaries, collects
the third-party license texts, and writes a `.sha256` checksum next to the
archive. The isolated home is required because Conan build flags are not part
of a package id.

| Command | Host requirements | Archive |
|---|---|---|
| `scripts/dist.sh linux` | GCC 16 and the native toolchain | `refrain-<version>-linux-x86_64.tar.gz` |
| `scripts/dist.sh mingw` | `x86_64-w64-mingw32-g++`; Wine for the smoke check | `refrain-<version>-windows-mingw-x86_64.zip` |
| `scripts/dist.sh msvc` | a Visual Studio developer prompt, or msvc-wine | `refrain-<version>-windows-msvc-x86_64.zip` |

The MinGW archive uses hardening without LTO: GCC 16's MinGW LTO miscompiles
exception handling across the static runtime, so a throwing worker task can
terminate the process instead of being contained. Linux keeps LTO. The Windows
archives are self-contained: the MinGW build is static and the MSVC archive
carries the app-local Visual C++ runtime DLLs. Archives are written to
`dist/` by default; `REFRAIN_DIST_OUT` selects another directory. Cross builds
need a native `refrain-tools` for the schema compiler; the MinGW target and the
msvc-wine path build it automatically.

One limitation: zstd and blake3 contain hand-written x86 assembly for their
SIMD kernels, and assembly objects carry no CET property notes. The final
executable therefore still lacks `IBT`/`SHSTK` even when every C/C++ object is
built with `-fcf-protection=full`; the property is an AND over all inputs.
Stack protector, FORTIFY and RELRO apply to the dependency code normally. A
CET-complete build needs those kernels disabled (a measurable compression and
hashing cost) or an explicit, carefully tested linker override.

The `refrain-bench` suite (median of three pinned runs, CPU-bound benchmarks,
the disk-heavy full-store case excluded) measures the following relative to a
plain `-O3` build:

| Configuration | Relative time |
|---|---|
| Plain `-O3` (reference) | — |
| Linux release build (`scripts/dist.sh linux`) | -4.6% |

The gain is concentrated in small allocations (-20% at 512 B) and the job pool
(-9%); the encode paths stay within one percent. The disk-heavy full-store case
is excluded because file I/O dominates its variance. Disable hardening when
profiling allocator internals:

```bash
cmake --preset conan-release -DREFRAIN_HARDEN=OFF -DREFRAIN_LTO=OFF
```

## Local iteration

The default configuration builds with LTO, which makes a one-file change pay a
full link. For the edit/build/test loop configure without it:

```bash
cmake --preset conan-release -DREFRAIN_LTO=OFF -DREFRAIN_ARCH_LEVEL=baseline
```

Benchmarks and release artifacts keep LTO and may raise the architecture level;
use a separate build directory so the two configurations do not thrash:

```bash
conan install . -pr gcc-release --build=missing -of build/bench \
    --lockfile=conan/locks/vendor.lock
cmake --preset conan-release -DREFRAIN_LTO=ON -DREFRAIN_ARCH_LEVEL=x86-64-v3 \
    -B build/bench
cmake --build build/bench --target refrain-bench-runtime
```

## Dependencies

`conan/locks/vendor.lock` pins every dependency and its revision. Changing a
dependency requires updating the lock and the matching in-repo recipe under
`conan/recipes/`. The vendor check in CI rejects a lock that does not match the
recipes.

## Troubleshooting

A missing preset means Conan has not produced the toolchain for this build
directory. Run `conan install` with the matching profile before configuring.

If the compiler is newer than the Conan profile expects, remove the profile and
run `scripts/bootstrap.sh --profile-only` to regenerate it.
