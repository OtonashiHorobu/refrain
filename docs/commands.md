# Commands

Refrain implements the in-game command surface as a small framework plus a
registration site per command. The framework owns the command graph, the typed
argument parsers, execution and the client-facing command tree and completion.
Gameplay code only registers commands and their executors; it never touches the
protocol representation.

The console has its own line parser (`src/server/console_commands.cpp`) because
its grammar is a server convenience, not the reference's command syntax. This
guide describes the in-game surface.

## Layers

| Layer | Header | Responsibility |
|---|---|---|
| Reader | `refrain/command/reader.hpp` | byte cursor over one command line, quoted strings and token boundaries |
| Argument parsers | `refrain/command/argument.hpp`, `parsers.hpp` | typed value extraction, parse errors with a byte offset, bounded completion |
| Dispatcher | `refrain/command/dispatcher.hpp` | registration, typed execution, immutable tree snapshots, completion |
| Context | `refrain/command/context.hpp` | typed argument access, feedback, failure reporting |
| Tree snapshot | `refrain/command/tree.hpp` | wire-oriented copy of the permitted graph for one command source |
| Wire adapter | `refrain/server/command_tree_wire.hpp` | maps a snapshot to the clientbound `commands` payload |
| Bridge | `refrain/server/command_bridge.hpp` | connects the network command callbacks to a dispatcher snapshot, completion and execution |

Parsers, the dispatcher and tree snapshots are independent of the network and
world layers. The bridge is the only place that knows both a command source and
a network connection.

## Argument parsers

Every parser implements the `argument_parser` interface: a `descriptor()` that
describes the argument for the client tree, a `parse()` that returns a typed
value or an `argument_error` (message plus byte offset from the argument start),
and a `suggest()` that adds bounded completions for a partial argument. A parser
may consume more than one whitespace-separated token; the dispatcher enforces
that the argument ends at a token boundary.

| Factory | Parsed value | Client argument type | Completion |
|---|---|---|---|
| `bool_arg()` | `bool` | `brigadier:bool` | `true`, `false` |
| `integer_arg()` | `std::int64_t` | `brigadier:integer` with optional bounds | bounds |
| `long_arg()` | `std::int64_t` | `brigadier:long` with optional bounds | bounds |
| `float_arg()` | `double` | `brigadier:float` with optional bounds | bounds |
| `double_arg()` | `double` | `brigadier:double` with optional bounds | bounds |
| `word_arg()` | `std::string_view` | `brigadier:string` single word | none |
| `quoted_arg()` | `std::string` | `brigadier:string` quotable phrase | none |
| `greedy_arg()` | `std::string_view` | `brigadier:string` greedy phrase | none |
| `enum_arg()` | canonical name `std::string` | `brigadier:string` greedy, server vocabulary | vocabulary with tooltips |
| `resource_location_arg()` | `data::resource_location` | `resource_location` | none |
| `block_position_arg()` | `block_position` | `block_pos` | `~` and `^` coordinate templates |
| `rotation_arg()` | `rotation` | `rotation` | yaw and pitch template |
| `player_name_arg()` | `std::string_view` | `game_profile` | online player names |
| `gamemode_arg()` | `game_mode` | `gamemode` | four modes with tooltips |
| `time_arg()` | `std::int64_t` ticks | `time` with minimum ticks | unit suffixes and clock markers |
| `dimension_arg()` | `data::resource_location` | `dimension` | installed dimension ids |
| `uuid_arg()` | `uuid` | `uuid` | none |

The dispatch tree carries the parser kind, not a protocol parser id: the wire
adapter owns the mapping from the framework taxonomy to the 26.3
`minecraft:command_argument_type` registry ids, so a parser change does not
renumber the protocol and a protocol change does not leak into gameplay code.

Completions are accumulated in a `suggestion_builder`, which caps the response
at 1024 entries, caps one suggestion and one tooltip at 4096 bytes on a UTF-8
boundary, drops empty and duplicate entries and reports truncation. A parser
without a meaningful vocabulary (`word`, `resource_location`, `uuid`) offers
nothing; the client simply shows no completion for that argument.

## Adding a command

Registration attaches literals and arguments to the dispatcher. `then()`
attaches a child, `executes()` attaches an executor, `require()` attaches a
visibility requirement, `tooltip()` supplies the completion tooltip and
`redirect()` continues parsing at another node without consuming its name.

```cpp
auto gamemode = dispatcher.literal("gamemode").tooltip("Change your game mode");
auto mode = dispatcher.argument("mode", command::gamemode_arg())
                .require(command::level::gamemode);
mode.executes([](command::context &ctx) -> refrain::result<int> {
    const auto mode = ctx.gamemode("mode");
    if (!mode.has_value()) {
        ctx.fail("Unknown game mode");
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "unknown game mode")};
    }
    ctx.reply(fmt::format("Set game mode to {}", command::to_string(*mode)));
    return 1;
});
gamemode.then(mode);
```

A new command needs no protocol, tree or suggestion plumbing: the next snapshot
contains it, the bridge serializes it and completion for its arguments comes
from the parsers. A new argument *type* needs a parser, a `parser_kind` value
and one entry in the wire adapter.

The dispatcher is single-writer: build the graph at startup, then share it.
`execute`, `suggest` and `tree` are const and can run concurrently.

## Client tree and completion

`dispatcher::tree(source)` walks the graph and returns an immutable snapshot in
preorder. Nodes whose requirement rejects the source are absent together with
their subtrees, and a redirect whose target is absent is dropped. The
visibility filter (see below) is consulted in the same walk. The snapshot owns
its storage, so it stays valid while the dispatcher is mutated; the bridge
caches one wire tree per permission level and dispatcher revision.

The wire adapter serializes the snapshot into the `commands` payload: node type
flags, children, redirect target, and per argument node the registry id and its
properties (bounds for the brigadier number arguments, the string shape, the
minimum tick count for `time`). Every argument node uses the client's
ask-server completion path: the server answers each suggestion request itself
and no custom suggestion provider is declared.

Completion resolves the argument being typed and asks the parsers of the
matching node for candidates. The response range starts at the first byte of
that argument, so filling it replaces a whole multi-token argument; this is what
lets a block position complete `1 2` to `1 2 ~`. The bridge copies the range and
the entries, including tooltips, into the clientbound
`command_suggestions` packet.

Literal completion is case-sensitive, matching literal execution. The reference
completes literals case-insensitively while executing them case-sensitively;
Refrain rejects a completion that the same input would not execute.

## Execution

`dispatcher::execute()` parses the whole command line against the graph, builds
a `context` with the typed argument values and their byte ranges, and calls the
executor. Executors return `void`, an `int`, a `status` or a
`result<int>`; failures are values. The context provides typed access
(`get<T>()`, `gamemode()`, `position()`, `rotation_value()`, `uuid_value()`,
`location()`, `string()`, `integer()`, `number()`, `boolean()`), feedback lines
through the source sink and a failure message the client can display. Feedback
is bounded per execution.

Parse failures carry the reason and a byte offset into the command line; the
dispatcher turns them into `"<reason> at position <byte>"` errors, which is the
form the bridge forwards to the client.

## Permissions

Two hooks control which commands a source sees and may run:

- `require()` on a node: a permission level or an arbitrary predicate. It
  filters the tree, completion and execution.
- `dispatcher::set_visibility_filter()`: one callback consulted for every node
  in addition to its own requirement. A permission service installs its check
  here; the command module itself has no permission registry.

The `source` interface carries the permission ladder, an opaque identity key and
an optional online-player list used for name completion, so a caller can
implement console, player, command-block or test sources without the network or
world layers. Whether a particular command requires a particular permission is
a registration decision; the reference's permission levels are available as
`command::level`.

## Current limits

- Entity selectors (`@a`, `@p`, `@s`, ...) are not part of the parser set yet,
  so player-name arguments accept a bare name only. The selector grammar and
  the entity argument family are a separate parser family.
- The reference's registry-backed arguments (block state, block predicate, item
  stack and predicate, nbt, component, style, message, particle, score holder,
  objective, team, slot, function, heightmap, resource or tag lookups, ranges)
  are not registered yet. They need a parser plus a provider for the values and
  are listed in the batches below rather than stubbed.
- Every argument is required: the tree has no optional-argument modifier, so a
  command that ends at a parent node is executable there instead (for example
  `/weather clear` and `/weather clear <duration>` are two executable nodes).
- All completion is server-side. The client is told to ask the server, which is
  correct for every current argument; the reference's named suggestion
  providers are for registries that do not exist here yet.
- The command tree carries no tooltips. Tooltips travel with the completion
  response, which is what the client shows.

## 26.3 command coverage

The table lists every root command the reference registers and the real state of
this tree. `implemented` means the reference's behaviour is reproduced for the
modelled world; `partial` means a subset of the command's subcommands, argument
forms or targets works; `planned` means nothing is registered yet. Planned
commands are delivered in the batches named in the notes.

| Command | Status | Notes |
|---|---|---|
| `/advancement` | planned | batch 4: execute and functions |
| `/attribute` | planned | batch 4: execute and functions |
| `/ban` | planned | batch 5: remaining administration |
| `/ban-ip` | planned | batch 5: remaining administration |
| `/banlist` | planned | batch 5: remaining administration |
| `/bossbar` | planned | batch 3: scoreboard and teams |
| `/chase` | planned | batch 1: player and world basics |
| `/clear` | planned | batch 2: blocks and entities |
| `/clone` | planned | batch 2: blocks and entities |
| `/compute` | planned | batch 5: remaining administration |
| `/damage` | planned | batch 2: blocks and entities |
| `/data` | planned | batch 4: execute and functions |
| `/datapack` | planned | batch 4: execute and functions |
| `/debug` | planned | batch 5: remaining administration |
| `/debugconfig` | planned | batch 5: remaining administration |
| `/debugmobspawning` | planned | batch 5: remaining administration |
| `/debugpath` | planned | batch 5: remaining administration |
| `/defaultgamemode` | implemented | Sets the default game mode of the world. |
| `/deop` | planned | batch 5: remaining administration |
| `/dialog` | planned | batch 1: player and world basics |
| `/difficulty` | partial | Numeric `0`-`3`; the reference uses the named levels. |
| `/effect` | planned | batch 1: player and world basics |
| `/enchant` | planned | batch 1: player and world basics |
| `/execute` | planned | batch 4: execute and functions |
| `/experience` | planned | batch 1: player and world basics |
| `/fetchprofile` | planned | batch 2: blocks and entities |
| `/fill` | planned | batch 2: blocks and entities |
| `/fillbiome` | planned | batch 2: blocks and entities |
| `/forceload` | planned | batch 4: execute and functions |
| `/function` | planned | batch 4: execute and functions |
| `/gamemode` | partial | Applies to the calling player only; the reference also accepts a target and has a query form. |
| `/gamerule` | planned | batch 4: execute and functions |
| `/give` | planned | batch 2: blocks and entities |
| `/help` | partial | Returns a fixed hint instead of generating usage from the tree. |
| `/item` | planned | batch 2: blocks and entities |
| `/jfr` | planned | batch 5: remaining administration |
| `/kick` | planned | batch 1: player and world basics |
| `/kill` | planned | batch 2: blocks and entities |
| `/list` | implemented | Lists the players currently in play. |
| `/locate` | planned | batch 4: execute and functions |
| `/loot` | planned | batch 2: blocks and entities |
| `/me` | planned | batch 1: player and world basics |
| `/msg` | planned | batch 1: player and world basics |
| `/op` | planned | batch 5: remaining administration |
| `/pardon` | planned | batch 5: remaining administration |
| `/pardon-ip` | planned | batch 5: remaining administration |
| `/particle` | planned | batch 2: blocks and entities |
| `/perf` | planned | batch 5: remaining administration |
| `/place` | planned | batch 2: blocks and entities |
| `/playsound` | planned | batch 2: blocks and entities |
| `/posteffect` | planned | batch 1: player and world basics |
| `/publish` | planned | batch 5: remaining administration |
| `/raid` | planned | batch 1: player and world basics |
| `/random` | planned | batch 4: execute and functions |
| `/recipe` | planned | batch 4: execute and functions |
| `/reload` | planned | batch 1: player and world basics |
| `/return` | planned | batch 4: execute and functions |
| `/ride` | planned | batch 2: blocks and entities |
| `/rotate` | planned | batch 2: blocks and entities |
| `/save-all` | planned | batch 1: player and world basics |
| `/save-off` | planned | batch 1: player and world basics |
| `/save-on` | planned | batch 1: player and world basics |
| `/say` | implemented | `/say <message>` broadcasts to every player. |
| `/schedule` | planned | batch 4: execute and functions |
| `/scoreboard` | planned | batch 3: scoreboard and teams |
| `/seed` | planned | batch 1: player and world basics |
| `/serverpack` | planned | batch 5: remaining administration |
| `/setblock` | planned | batch 2: blocks and entities |
| `/setidletimeout` | planned | batch 1: player and world basics |
| `/setworldspawn` | planned | batch 1: player and world basics |
| `/spawn_armor_trims` | planned | batch 1: player and world basics |
| `/spawnpoint` | planned | batch 1: player and world basics |
| `/spectate` | planned | batch 2: blocks and entities |
| `/spreadplayers` | planned | batch 2: blocks and entities |
| `/stop` | planned | batch 1: player and world basics |
| `/stopsound` | planned | batch 2: blocks and entities |
| `/stopwatch` | planned | batch 4: execute and functions |
| `/summon` | planned | batch 2: blocks and entities |
| `/swing` | planned | batch 2: blocks and entities |
| `/tag` | planned | batch 2: blocks and entities |
| `/team` | planned | batch 3: scoreboard and teams |
| `/teammsg` | planned | batch 3: scoreboard and teams |
| `/teleport` | planned | batch 2: blocks and entities |
| `/tellraw` | planned | batch 1: player and world basics |
| `/test` | planned | batch 5: remaining administration |
| `/tick` | planned | batch 4: execute and functions |
| `/time` | partial | Models the overworld clock only; `query`, `set`, `add`, `pause` and `resume` exist, the reference clock registry, rate and timeline forms do not. |
| `/title` | planned | batch 1: player and world basics |
| `/transfer` | planned | batch 5: remaining administration |
| `/trigger` | planned | batch 3: scoreboard and teams |
| `/unpublish` | planned | batch 5: remaining administration |
| `/version` | planned | batch 1: player and world basics |
| `/warden_spawn_tracker` | planned | batch 1: player and world basics |
| `/waypoint` | planned | batch 1: player and world basics |
| `/weather` | partial | Single overworld weather state; the duration is unbounded where the reference bounds it. |
| `/whitelist` | planned | batch 5: remaining administration |
| `/worldborder` | planned | batch 4: execute and functions |

Two server commands are not part of the reference's root set: `/mods` manages
the installed mod packages (operator-gated tree) and `/hardcore` toggles the
hardcore flag of the open world.

