# Contributing to Refrain

Refrain reproduces the behavior of a proprietary game. A contribution that
implements vanilla behavior starts by reading the complete relevant source of
the target version (26.3, official mappings) and recording what was read in a
specification. Code is written from that specification, not copied or
translated from the source. Game assets are never committed; data derived from
Minecraft lives only under `content/vanilla/` and is covered by its own notice,
not by the repository license.

## Workflow

1. Open an issue describing the behavior you intend to add or change.
2. Build and test locally as described in [docs/building.md](docs/building.md)
   and [docs/testing.md](docs/testing.md).
3. Add tests for new behavior. Negative cases matter more than happy paths.
4. Update the documentation that the change makes stale.
5. Open a pull request with a subject in the commit convention below.

## Coding rules

- C++26, GCC 16 on Linux x86_64. Keep the code warning-clean under the project
  warning set.
- Errors are values. Public APIs return `refrain::result` or `refrain::status`
  and never throw. Internal code may use exceptions only where an existing
  boundary already catches them.
- Use `fmt` for formatting. Do not mix `std::format`, `iostream` formatting or
  `sprintf` into the same path.
- The build uses `-fno-rtti`.
- Operating-system calls belong in `src/platform/{posix,windows}`. Upper layers
  use `refrain::platform` or `refrain::core::io`.
- Third-party dependencies are declared in `conanfile.py` and pinned in
  `conan/locks/vendor.lock`. Do not add bundled copies or fetch content at
  configure time.
- Files under `sdk/generated/` are produced by `refrain-tools schema gen`.
  Change the schema or the generator, then regenerate. Do not edit generated
  headers by hand.
- Run `clang-format` on changed C++ files. CI rejects formatting drift.

## Commit convention

Commits follow [Conventional Commits](https://www.conventionalcommits.org/):

```text
type(scope): short imperative summary

Optional body explaining the behavior change and its tradeoffs.
```

Accepted types are `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`,
`ci` and `chore`. Scope is a module name such as `world`, `sim`, `data`,
`schema` or `server`. Subject lines stay under 72 characters and do not end
with a period.

Commit messages describe the resulting behavior. They do not reference
internal planning documents, task identifiers, review threads or dates.

## Tests and fixtures

Fixtures must be synthetic or generated from data you own. Do not commit game
jars, extracted assets, world saves or logs. Train and dictionary data must be
produced by the repository tooling so it can be regenerated.

## AI assistance

Parts of this repository are written with AI assistance and reviewed by
maintainers. Contributors using such tools remain responsible for every line
they submit: they must be able to explain the change, its failure modes and
its tests. User-facing prose, release notes and security responses are written
and reviewed by people.

## License

Contributions are accepted under the MIT License. By opening a pull request you
confirm that you have the right to submit the work under that license.
