# Agent and contributor guide

Refrain is a C++26 reimplementation of a Minecraft Java Edition 26.3 server.
The repository ships code, contracts, tests, user documentation and the
game-derived tables under `content/vanilla/`; it ships no game code, no game
assets and no datapack. Those tables carry their own notice and are not covered
by the repository license; the datapack is generated locally from the user's
own `server.jar`.

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
live in source files. The game-derived tables live only under
`content/vanilla/`, which carries its own notice and is not covered by the
repository license; the datapack is generated locally and never committed.
Test fixtures that do not derive from the game stay synthetic.

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
| `content/vanilla/` | derived game tables with their own notice, outside the repository license |
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
- An incomplete or placeholder implementation must be marked at the exact
  site with a `TODO:` or `FIXME:` comment that says what is missing and what
  the correct behavior is. A stub that silently returns a default, ignores an
  input or hard-codes one value instead of implementing the general case is a
  defect, not a finished feature. The marker states the gap in one or two
  sentences; it never carries a ticket number, milestone label or date.

## Local workflow

- Configure the local build for fast iteration:
  `cmake --preset conan-release -DREFRAIN_LTO=OFF -DREFRAIN_ARCH_LEVEL=baseline`.
  LTO and a raised architecture level belong to release artifacts and to
  benchmarks, not to the edit/build/test loop; a single-file change must stay
  incremental.
- Measure in a separate build directory (for example `build/bench`) configured
  with `-DREFRAIN_LTO=ON -DREFRAIN_ARCH_LEVEL=x86-64-v3`, and say which
  directory produced a number.
- Pull requests run the fast gates: the Linux build, the vendor guard, the
  formatting check and the DCO check. The sanitizer builds, the Windows
  toolchains and the CodeQL scan run on the default branch and on a schedule.
  Do not wait for them before opening or updating a pull request; when one
  fails on the default branch, fix it there.
- One topic per pull request, and the branch keeps the build green before it
  merges.
- Every commit that changes behavior is the moment to look at the tracker:
  update the issue it belongs to (comment with what landed, tick the
  checklists, close what is finished, reopen what the commit only half does)
  and check the milestone totals. A merged change with no issue update leaves
  the tracker wrong for the next reader.

## Determinism

Anything that feeds the state hash, a replay or a golden capture must iterate
in a state-derived order: ascending entity id, ascending chunk position, sorted
keys. Hash-map order and creation order are not evidence of anything and must
never decide the order in which a system mutates state, consumes randomness or
emits packets. A subsystem that cannot satisfy this runs on the simulation
thread and documents the exception next to the code.

## Logging

Every runtime log line is a sentence written for the operator, with the emitting
component as its implicit subject. A reader who has never seen the source must
be able to say what happened, to which object, and whether anything still needs
attention.

- Tag every line with its subsystem through a per-module logger
  (`refrain::log::logger`): `[12:00:00] [main/info] [mod-host]: loaded 11 mods`.
  Module names are lowercase and hyphen-separated (`mod-host`, `net-transport`);
  never repeat the module name in the message body.
- Start the message with a lowercase letter and leave the period off. Proper
  nouns keep their case (`Minecraft`, `Asio`, `zstd`).
- Use the tense that matches the state: `-ing` for an action that has started
  and is observable, `-ed` for a finished action, `failed to <verb> <object>`
  for a failure, `received <object> from <actor>` for input, and
  `<verb>ing/<verb>ed <object> because <reason>` for a decision.
- Name the object: the path, the id, the coordinate, the count. Counts are
  `<number> <noun>` with a correct plural (`49 chunks`, never `chunk(s)`);
  units attach to the number (`6.8s`, `255MiB`, `60000ms`).
- One line, one event, one sentence. Put details after commas, never in
  parentheses. Do not dump bare `key=value` pairs as a message.
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
