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

## Mod profiles

A build profile selects which first-party mods exist in the binary. It is a
build-time choice: the compiled mod code, the built-in content image and the
default gamepack all derive from one list in `profiles/<name>.toml`, so the
three cannot drift apart. The gamepack remains the runtime choice of which
providers are active inside that set; it can never select a mod the binary was
not built with.

| Profile | Mods | Default gamepack | Purpose |
|---|---|---|---|
| `vanilla` | all 12 bundled vanilla mods | `gamepack/gamepack.toml` | default build and release |
| `minimal` | content + data layer only | `gamepack/presets/data-only.toml` | content tooling and mod-host smoke |
| `dynamic` | vanilla set, data layer as shared module | `gamepack/gamepack.toml` | native-loader smoke |
| `1.17` | content + data layer (example) | `gamepack/presets/data-only.toml` | template for a version-specific set |

Select a profile through the Conan option, which seeds the CMake cache:

```bash
conan install . -pr gcc-release --build=missing \
    -o "&:mod_profile=minimal" --lockfile=conan/locks/vendor.lock
cmake --preset conan-release
cmake --build --preset conan-release
```

An explicit `-DREFRAIN_MOD_PROFILE=<name>` on the configure line wins over the
Conan option, so an existing build directory can switch profiles without
re-running Conan:

```bash
cmake --preset conan-release -DREFRAIN_MOD_PROFILE=minimal
```

The remaining cache variables narrow the selection:

| Variable | Effect |
|---|---|
| `REFRAIN_MOD_PROFILE` | profile file to read (`profiles/<name>.toml`), default `vanilla` |
| `REFRAIN_MODS` | comma-separated mod ids overriding the profile's list; `REFRAIN_MOD_GAMEPACK` must then name a matching gamepack |
| `REFRAIN_MOD_DYNAMIC` | comma-separated ids to build as shared modules, overriding the profile's `dynamic` list |
| `REFRAIN_MOD_GAMEPACK` | default gamepack override (empty: the profile's gamepack) |
| `REFRAIN_MOD_LINK` | `auto`, `static`, `mixed` or `dynamic`; `auto` is static unless the profile lists dynamic mods |

A profile is validated at configure time: a listed mod must have a directory and
manifest under `mods/`, and the default gamepack must pin exactly the profile's
mod set. A profile that lists an unknown mod, or a gamepack that pins a mod the
profile does not build, fails the configure step with both names instead of
producing a binary that cannot start.

`scripts/mod-profile-smoke.sh <profile>` builds one profile and runs its checks:
profile/build identity from `refrain-tools version`, `mods check --profile
<name> --built`, the gamepack check, an independently regenerated built-in image
compared byte-for-byte, and a composition start. CI runs it for `minimal` and
`dynamic`; the default `vanilla` job builds and tests the full suite.

Dynamic profiles build the selected mods as shared modules under
`<build>/bin/modules`. Only ELF targets are supported; the module links against
no project library and resolves the host runtime from the executable, which is
linked with `-rdynamic`. On startup the loader opens every module this binary
was built with, validates its descriptor and registers its factory before the
composition loads, so the mod behaves like a statically linked one.

The runtime objects the module calls must be part of the host executable. A
module that references a runtime function no host code pulls in fails to load
with an undefined-symbol diagnostic; a shared runtime library that removes that
restriction is not built yet. `REFRAIN_MOD_LINK=dynamic` (build every mod as a
module) therefore fails at configure time; select the modules explicitly with a
profile's `dynamic` list instead.

The resolved profile is written to `<build>/generated/mod_profile.toml` with the
profile name, link mode, mod list and a build id. The build id and the profile
name are embedded in the binary (`refrain-tools version` prints them, the server
startup log names them), recorded in `mods.lock` and copied into a release
archive as `profile.toml`. A `mods.lock` written by a different profile or build
is refused instead of being silently compared against this binary.

Release packaging follows the profile. `REFRAIN_DIST_MOD_PROFILE` selects it:

```bash
REFRAIN_DIST_MOD_PROFILE=minimal scripts/dist.sh linux
```

The archive contains the profile's default gamepack, `profile.toml` and, for a
dynamic profile, the module directory next to the server binary.

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
`dist/` by default; `REFRAIN_DIST_ARCH_LEVEL` selects the code-generation level (default `baseline`); release archives normally stay at the portable baseline, and a package for known hardware may name `x86-64-v3` or another level from the CMake option table. `REFRAIN_DIST_OUT` selects another directory. Cross builds
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
cmake --build build/bench --target refrain-bench-runtime refrain-bench-light
```

### Light engine benchmark

`refrain-bench-light` measures the light engine through its own counters
(`sim::light_engine::stats()`) instead of timing a benchmark body from the
outside. It generates a deterministic square of overworld columns with the
production chunk generator, adopts the columns into a fresh in-memory world in
ascending position order, and then runs a deterministic block-edit storm. The
data comes from the same extracted datapack the runtime generator benchmark
uses; without it the binary prints a skip line and exits successfully.

```bash
REFRAIN_VANILLA_WORLDGEN=content/vanilla/26.3/data \
    build/Release/bin/refrain-bench-light --chunks 64 --edits 8 --repeat 3
```

`--chunks` is rounded up to the nearest square because the production
generator works on square tiles: `--chunks 64` is one 8×8 tile. The tile edge
is 8 chunks, the production unit; a smaller `--tile-size` makes a smoke run
cheaper but makes the chunk-ready numbers less representative.

| Option | Effect |
|---|---|
| `--data-root` | datapack `data` directory; defaults to `REFRAIN_VANILLA_WORLDGEN` |
| `--tables` | content table container; defaults to `REFRAIN_VANILLA_TABLES`, else `<data-root>/../tables.rvtb` |
| `--templates-root` | structure templates; defaults to `REFRAIN_VANILLA_STRUCTURES` |
| `--biome` | fixed biome; empty (default) runs the dimension's biome source like a production world |
| `--chunks` | columns to generate and adopt, rounded up to a square (default 64) |
| `--tile-size` | generator tile edge in chunks (default 8, the production unit) |
| `--edits` | torch place/break edits per column (default 8) |
| `--repeat` | workload repeats, each on a fresh world (default 1) |
| `--target` | `full` (default, the served chunk path) or `terrain` |

Each repeat prints `RESULT scenario=...` lines. The edit storm places a torch
in the air cell above the highest block of every chunk's centre column and
breaks it again, so every edit changes the cell and exercises both the increase
and the decrease path. Read the fields as follows:

- `columns_lit` is the number of columns whose initial light pass ran;
  `initial_nodes` and `initial_nodes_per_column` are the queue entries that
  pass processed, `initial_ns_per_column` is its share of `initial_ns`.
  `initial_peak_pending` is the highest queue length the initial passes
  seeded.
- `generate_ms`, `convert_ms` and `adopt_ms` decompose the chunk-ready path:
  tile generation, the storage bridge conversion and the world adoption that
  runs the initial light pass. `chunk_ready_ms_per_column` is their sum over
  the columns, light included: what a client waits for before the chunk packet
  can carry light.
- `drain_nodes` and `drain_nodes_per_edit` are the queue entries the
  incremental edit drains processed; `drain_ns_per_edit` is their share of
  `drain_ns`.
- `pending_peak` is the highest queue length observed while the phase drained,
  so it shows how close the workload came to the drain budget.
- `tick_backstop_nodes` is the work the tick's backstop drain still had left
  after the edit storm; a non-zero value means an edit left queued work behind.

A number from a machine that is compiling or serving is a smoke test, not a
baseline. Compare runs only from the same idle benchmark host; changing the host
re-baselines every number.

### Benchmarking on a separate host

A benchmark measured while the same machine compiles something else is not
comparable: another job moves the numbers further than most changes do. Point
`scripts/dev/bench-remote.sh` at an idle machine and it syncs the working tree
over SSH, builds there with LTO and the raised architecture level, runs the
benchmark and prints the result:

```bash
REFRAIN_BENCH_HOST=user@host scripts/dev/bench-remote.sh \
    --targets terrain,full --chunks 16 --workers 1,8 --repeat 5
```

The script builds and runs `refrain-bench-runtime` by default. Set
`REFRAIN_BENCH_TARGET=refrain-bench-light` to run the light engine benchmark
instead; its arguments pass through unchanged:

```bash
REFRAIN_BENCH_HOST=user@host REFRAIN_BENCH_TARGET=refrain-bench-light \
    scripts/dev/bench-remote.sh --chunks 64 --edits 8 --repeat 3
```

The host comes from the environment and is never committed. When the machine is
unreachable the script warns and falls back to a local `build/bench`; a number
from that fallback shares the machine with everything else, so say so when
reporting it. `REFRAIN_BENCH_REQUIRE=1` turns the fallback into an error.

Changing the benchmark host re-baselines every number: measurements from
different machines cannot be compared.

## Dependencies

`conan/locks/vendor.lock` pins every dependency and its revision. Changing a
dependency requires updating the lock and the matching in-repo recipe under
`conan/recipes/`. The vendor check in CI rejects a lock that does not match the
recipes.

GitHub Actions versions are updated by Dependabot from
`.github/dependabot.yml`. Conan is not one of Dependabot's supported ecosystems,
so its dependencies are not updated by Dependabot: run
`python3 tools/dep_audit.py` to compare the pinned versions with the upstream
ones, or enable Renovate if an automated pull request per update is wanted
(Renovate reads `conanfile.py` and `conan.lock`).

Vulnerability alerts for Conan packages come from the ConanCenter audit and
appear under Security -> Code scanning, not under Dependabot alerts. The
`dependency-audit` workflow runs weekly: it authenticates with the repository
secret `CONAN_AUDIT_TOKEN` (register at <https://conan.io/audit/register>),
scans the lockfile, and uploads a SARIF report. In-repo recipes under
`conan/recipes/` are not in ConanCenter and are therefore not covered by that
audit; review their upstream advisories manually when bumping them.

Findings that were checked against the vendored implementation are recorded in
`.github/conan-audit-triage.json`: `not-applicable` and `accepted-risk` entries
are left out of the alert stream and listed in the job summary, while
`pending-fix` entries stay visible with their note. Do not dismiss an alert
without recording the reason there, so the next run does not recreate it.

## Troubleshooting

A missing preset means Conan has not produced the toolchain for this build
directory. Run `conan install` with the matching profile before configuring.

If the compiler is newer than the Conan profile expects, remove the profile and
run `scripts/bootstrap.sh --profile-only` to regenerate it.

## See also

- [Running a server](running.md) for the command lines of the built binaries.
- [Testing](testing.md) for the test layout, the local CI script and the soak
  and load tools that use a build directory.
