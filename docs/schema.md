# Schema compiler

Refrain describes its data and contracts in two schema languages. RSC describes
data layouts and registry content; RIDL describes service contracts. The
`refrain-tools schema gen` command compiles both into headers under
`sdk/generated/`.

## Sources

| Language | Location | Purpose |
|---|---|---|
| RSC | `schema/` | data structs, enums, packets, records, handles |
| RIDL | `sdk/idl/` | service contracts, events, method metadata |

Both dialects share a parser and a semantic analyzer. A file starts with a
module, a version and imports:

```ridl
module refrain::services::world;
version 1.0.0;

import refrain::core;
```

## Declarations

| Declaration | Meaning |
|---|---|
| `struct` | value type with a fixed layout |
| `enum` | unsigned integer with named values |
| `flags` | bit set over an unsigned integer |
| `record` | data record with a stable identity |
| `packet` | protocol message with an explicit identifier |
| `handle` | opaque typed handle |
| `event` | transient in-process notification |
| `service` | contract with methods and composition metadata |

Fields use scalar types (`i8`–`i64`, `u8`–`u64`, `bool`, `f32`, `f64`), or
`string`, `bytes`, `array<T>`, `slice<T>`, `result<T, E>` and handles. A field
inherits a default `struct_size` prologue in generated ABI structs.

A service block declares its version, side, thread affinity, composition mode,
provided and required services, and capabilities:

```ridl
service world : 1.0.0 {
    side = server;
    thread = sim;
    provides = [ "refrain:world" ];
    requires = [ "refrain:core>=1.0.0" ];
    capabilities = [ "block-access" ];

    #[thread(sim)] #[can_fail]
    fn block_at(in block_pos pos) -> u32;
}
```

Methods may carry attributes such as `#[thread(sim)]`, `#[can_fail]`,
`#[borrow]`, `#[owned]`, `#[caller_provided]` and `#[hot_path]`. A method
annotated `#[schedule(interval=N, phase=pre_tick|post_tick)]` must take no
parameters and return `void` or `result<void, E>`; the generator emits a
descriptor table that the host scheduler runs on the tick grid.

Events are declared at module scope. Their fields are limited to scalars,
`string`, `bytes` and `slice<scalar>`. An event annotated `#[batch]` carries
exactly one `slice<scalar>` field. The generator emits a deterministic layout,
a stable string id and a trait specialization used for compile-time checks.

## Generation

```bash
build/Release/bin/refrain-tools schema gen
build/Release/bin/refrain-tools schema gen --check
```

Generation writes C ABI headers under `sdk/generated/c/`, C++ wrappers under
`sdk/generated/cpp/` and reflection metadata in
`sdk/generated/cpp/refrain/reflection.hpp`. Every file records the schema hash
of the inputs. `--check` regenerates into a temporary directory and fails when
the committed output differs.

Generated files are not edited by hand. Change the schema or the emitter in
`tools/schema/`, then regenerate.

## Versioning

Contract versions follow SemVer. The ABI major version must match between a
provider and its consumer; the minor version may be newer at the provider.
During the pre-release period all service and format versions remain `1.0.0`
and incompatible changes update the generated output and the schema hash rather
than introducing migration paths. Published releases switch to semantic version
increments with migrations.
