# Agent and contributor guide

Refrain is a C++26 reimplementation of a Minecraft Java Edition 26.3 server.
The repository ships code, contracts, tests and user documentation; it ships no
game code and no game assets. Data derived from the game lives only under
`content/vanilla/`, carries its own notice and is not covered by the repository
license.

## Ground rules

Read the surrounding code, tests and documentation before changing behavior.
Prefer the smallest change that solves the problem, and keep unrelated
refactors out of the same commit.

Every feature that reproduces vanilla behavior starts by reading the complete
relevant source of the target version (26.3, official mappings): the entry
point, the full call chain, every branch, the data it reads, where it consumes
randomness and in what order, and the order of its writes. Read the target
version itself; summaries, older versions and memory are not evidence. Record
the files and call chains read in the feature's specification before writing
code, and review the implementation against that specification.

Code is written independently from the specification. Do not paste vanilla
source into this tree and do not translate it line by line. Game assets never
live in source files; data derived from Minecraft lives only under
`content/vanilla/`, which carries its own notice and is not covered by the
repository license. Test fixtures that do not derive from the game stay
synthetic.

## Repository map

| Path | Contents |
|---|---|
| `include/refrain/` | public headers, grouped by module |
| `src/` | module implementations |
| `sdk/idl/`, `sdk/generated/` | RIDL contracts and generated C/C++ headers |
| `schema/` | RSC data schemas |
| `test/` | Boost.UT suites and synthetic fixtures |
| `bench/` | Google Benchmark suites |
| `tools/` | schema compiler, vendor tooling and repository checks |
| `cmake/`, `conan/` | build modules, in-repo recipes and the dependency lock |
| `mods/`, `gamepack/` | bundled mod manifests and the default composition |
| `content/vanilla/` | game-derived data with its own notice, outside the repository license |
| `docs/` | architecture, build, run, test, schema and modding guides |

## Build and test

```bash
scripts/bootstrap.sh
```

The script installs dependencies, configures the `conan-release` preset and
runs the test suite. For the manual sequence and options, read
[docs/building.md](docs/building.md).

```bash
cmake --build --preset conan-release
./build/Release/bin/refrain-tests
ctest --preset conan-release --output-on-failure
scripts/ci.sh --offline
```

`scripts/ci.sh` runs the vendor checks, schema drift check, C ABI compile
check, unit tests, CLI contract, runtime smoke, extraction smoke, crash
reporter smoke and the formatting gate.

## Code rules

- C++26, GCC 16 on Linux x86_64. The build uses `-fno-rtti` and treats warnings
  as errors.
- Errors are values: return `refrain::result` or `refrain::status`. Exceptions
  must not cross a public API or a contract boundary.
- Use `fmt` for formatting. Do not mix `std::format`, `iostream` formatting or
  `sprintf` into the same path.
- Operating-system calls belong in `src/platform/{posix,windows}`. Other layers
  use `refrain::platform` or `refrain::core::io`.
- Dependencies are declared in `conanfile.py` and pinned in
  `conan/locks/vendor.lock`. Do not add bundled copies or download sources at
  configure time.
- Run `clang-format` on changed C++ files. Generated headers under
  `sdk/generated/` are never reformatted or edited by hand.

## Logging

Runtime logs record operator-visible facts, not how the code works. Keep
info/warn/error lines short and self-contained:

- Use `component: fact` and `key=value` for identifiers, counts and paths.
- Do not add parenthetical asides, rationale or mechanism descriptions; a
  reader should not need the source to understand the line.
- Put the cause of a failure in the returned error message instead of a second
  log line.
- Debug logs may carry detail; info/warn/error stay terse.

## Documentation and commits

Public prose and code comments are English and self-contained. State the
invariant, the failure mode or the reason; do not cite planning documents,
ticket numbers, milestone labels, dated review notes or local paths. A reader
of the repository should not need any material outside it.

Commit messages follow Conventional Commits and use English:

```text
type(scope): imperative summary
```

Accepted types are `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`,
`ci` and `chore`. Keep the subject under 72 characters. Add a body only when the
behavior change needs context; never describe process, schedules or review
history.

Commits are signed off with `git commit -s`; the `Signed-off-by` trailer is
required, must match the commit author, and certifies the Developer Certificate
of Origin 1.1. A pull request with an unsigned commit is not merged.

## Generated files

`sdk/generated/` is produced by the schema compiler:

```bash
build/Release/bin/refrain-tools schema gen
build/Release/bin/refrain-tools schema gen --check
```

Change the sources under `sdk/idl/`, `schema/` or the emitter in `tools/schema/`,
then regenerate. A drift check runs in CI.

## Tests and fixtures

New behavior needs a test, and negative cases matter more than a second happy
path. Test names describe the behavior, not the change that introduced it.
Golden files under `test/fixtures/golden/` freeze output; update them only for
an intentional contract change and review the diff.

## Security

Report vulnerabilities through the private channel described in
[SECURITY.md](SECURITY.md). Do not open a public issue for a security problem.
