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
