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

Windows and macOS builds are not supported. A MinGW cross build of the
libraries, tools and server is possible but untested at runtime.

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

Optimized builds use `-O3 -DNDEBUG`. Both hardening layers and LTO are
opt-in: the regular CI build and local development builds stay fast and
unhardened, while release archives use the hardened profile described below.

| Option | Default | Effect |
|---|---|---|
| `REFRAIN_HARDEN` | OFF | Stack protector and stack-clash protection, CET (`-fcf-protection=full`) on x86, hidden symbol visibility, `_FORTIFY_SOURCE=3`, and ELF `RELRO`/`BIND_NOW`/non-executable stack. MSVC uses `/guard:cf`; MinGW adds `--dynamicbase --nxcompat --high-entropy-va`. |
| `REFRAIN_HARDEN_MAX` | OFF | Adds `-ftrivial-auto-var-init=zero` and `-fzero-call-used-regs=used-gpr` on top of the baseline. |
| `REFRAIN_LTO` | OFF | Link-time optimization for optimized GCC/Clang builds (the Conan `lto` option feeds it). Sanitizer builds and MSVC skip LTO automatically. |

Only first-party targets are rebuilt with these flags; third-party libraries
come prebuilt from Conan. The flags apply to `Release`, `RelWithDebInfo` and
`MinSizeRel`; `Debug` keeps the stack protector but skips FORTIFY and LTO.

`scripts/dist.sh` is the release path: it rebuilds every Conan dependency from
source with the same hardening flags (plus `-ffat-lto-objects -flto=auto`) in an
isolated Conan home, then builds the server and tools with
`REFRAIN_HARDEN=ON REFRAIN_HARDEN_MAX=ON REFRAIN_LTO=ON`. The isolated home is
required because Conan build flags are not part of a package id.

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
| `REFRAIN_HARDEN=ON`, LTO off | +1.6% |
| `REFRAIN_HARDEN=ON`, LTO on | -5.8% |
| `REFRAIN_HARDEN_MAX=ON`, LTO on | -5.2% |
| All dependencies hardened + LTO (`scripts/dist.sh`) | -8% to -13% |

The baseline hardening is therefore close to free once LTO is enabled; the
maximum layer costs roughly one percent more. Small-allocation microbenchmarks
show a wider spread (up to +8% for the baseline layer, recovered by LTO), so
disable hardening when profiling allocator internals:

```bash
cmake --preset conan-release -DREFRAIN_HARDEN=OFF -DREFRAIN_LTO=OFF
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
