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
| `extension` | optional provider capability attached to one service |

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

## Extensions

An `extension` declares an optional capability that providers may attach to a
base service without changing the service's existing method slots:

```ridl
extension chunk_codec : 1.0.0 {
    extends = "refrain:net.protocol";
    capability = "chunk-codec";

    #[thread(worker)] #[can_fail]
    fn encode_chunk(in chunk_view chunk, out chunk_buffer encoded)
        -> result<void, protocol_error>;
}

service net_protocol : 1.1.0 {
    extensions = [ chunk_codec ];
    // existing methods remain unchanged
}
```

`extends` names the parent service by one of its `provides` entries;
`capability` is a lowercase label (`[a-z0-9]` followed by `[a-z0-9._-]`, at
most 64 characters). An extension body accepts `fn` methods with the same
parameter, return and `result<T, E>` rules as service methods. `#[schedule]`
is rejected on extension methods: extension vtables have no tick table.

The stable extension id is derived deterministically as
`<extends>/<name>`; the example above is
`refrain:net.protocol/chunk_codec`. The id is emitted as a C macro, a C++
constant and reflection metadata. Extension declarations must be listed by
their parent service's `extensions` list; a service may list an extension from
an imported module. Duplicate extension ids and duplicate entries in one
service's list are errors.

Extension versions are full SemVer 2.0.0 release versions (like service
versions). Their major and minor components must fit 16 bits because the query
slot packs the requested minimum as `(major << 16) | minor`. A provider is
compatible when its major equals the requested major and its minor is at least
the requested minor; patch is informational. A newer provider minor is
accepted, a different major or an older minor returns
`REFRAIN_EXTENSION_VERSION_MISMATCH`.

### Generated ABI

A service that declares `extensions` appends one slot to its vtable, after the
contract method slots:

```c
refrain_status (*query_extension)(void *self, refrain_str id,
                                  uint32_t min_version,
                                  refrain_extension_ref *out);
```

The same header defines `refrain_extension_ref` (a `struct_size` prologue,
the provider version, the stable id, and type-erased `vtable`/`self` pointers)
together with `REFRAIN_EXTENSION_VERSION(major, minor)` and the status codes
`REFRAIN_EXTENSION_NOT_FOUND`, `REFRAIN_EXTENSION_VERSION_MISMATCH` and
`REFRAIN_EXTENSION_INVALID`. Each declared extension also gets a C vtable
(`<prefix>_vtable` with a leading `uint32_t struct_size`) and metadata macros
for its id, version, capability and parent. The host initialises
`ref->struct_size` before calling `query_extension`; a producer that returns a
different reference size, a mismatched id, an incompatible version or an
undersized extension vtable is rejected as malformed.

### Provider registration

The generated C++ header contains id-keyed registration helpers. A provider
builds one binding per extension and forwards the query slot; dispatch,
version comparison and struct-size validation are generated:

```cpp
const net_protocol::chunk_codec::vtable codec_table{
    REFRAIN_SERVICES_NET_PROTOCOL_CHUNK_CODEC_VTABLE_SIZE, &encode_chunk};

const net_protocol::extension_binding bindings[] = {
    net_protocol::make_extension_binding<net_protocol::chunk_codec>(
        codec_table, codec_state),
};

struct my_provider {
    net_protocol::extension_registry extensions{std::span{bindings}};
    // base provider state
};

my_provider state;
refrain_services_net_protocol_vtable vtable{};
// fill struct_size, the ABI version words and the service method slots
vtable.query_extension =
    &net_protocol::query_extension<my_provider>;
const net_protocol::provider service{vtable, &state};
```

Extensions are owned by the base provider: their bindings, vtables and
`self` pointers must stay valid for the provider's lifetime, and there is no
independent unload. A missing extension returns `not_found`; a version that
cannot satisfy the request returns `version_mismatch`; a malformed or
undersized reference fails closed with `invalid_argument`. The typed binding
performs these checks once:

```cpp
const auto codec = service.extension<net_protocol::chunk_codec>();
if (codec) {
    refrain::core::chunk_buffer buffer{};
    const auto encoded = codec->encode_chunk(view, buffer);
    // ...
}
```

`extension<T>()` checks the id, version, reference size and vtable size at
binding time; subsequent calls dispatch directly through the extension vtable.
The wrapper works with `-fno-rtti`.

### Compatibility

Services without an `extensions` list keep their exact previous vtable layout
and generated output, apart from the schema hash in the banner. Adding an
extension point appends the query slot: bump the service's minor version and
rebuild providers. Providers built against the older vtable are rejected
before the slot is called because their `struct_size` is too small.

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
During the pre-release period contract changes update the generated output and
the schema hash rather than introducing migration paths. Additive extension
points bump the service minor version (for example `net_protocol` 1.1.0) while
keeping the same major; published releases switch to semantic version
increments with migrations.
