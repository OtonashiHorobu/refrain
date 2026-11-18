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
| Selector values | `refrain/command/selector.hpp` | context-free entity selector value, candidate contract and resolver |
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
| `vec3_arg()` | `vec3_position` | `vec3` | `~` and `^` coordinate templates |
| `rotation_arg()` | `rotation` | `rotation` | yaw and pitch template |
| `player_name_arg()` | `std::string_view` | `game_profile` | online player names |
| `gamemode_arg()` | `game_mode` | `gamemode` | four modes with tooltips |
| `time_arg()` | `std::int64_t` ticks | `time` with minimum ticks | unit suffixes and clock markers |
| `dimension_arg()` | `data::resource_location` | `dimension` | installed dimension ids |
| `uuid_arg()` | `uuid` | `uuid` | none |
| `entity_selector_arg()` | `entity_selector` | `entity` with the argument-form flag byte | `@p`, `@a`, `@r`, `@s`, `@e`, `@n`, player names, selector option keys and values |

The dispatch tree carries the parser kind, not a protocol parser id: the wire
adapter owns the mapping from the framework taxonomy to the 26.3
`minecraft:command_argument_type` registry ids, so a parser change does not
renumber the protocol and a protocol change does not leak into gameplay code.

Completions are accumulated in a `suggestion_builder`, which caps the response
at 1024 entries, caps one suggestion and one tooltip at 4096 bytes on a UTF-8
boundary, drops empty and duplicate entries and reports truncation. A parser
without a meaningful vocabulary (`word`, `resource_location`, `uuid`) offers
nothing; the client simply shows no completion for that argument.

## Entity selectors

`entity_selector_arg()` is the reference's `EntityArgument`: a bare player name
or one of six selector families, optionally narrowed by a filter list. The
parser produces a context-free `entity_selector`; resolving it against the
command source and the online players happens in the executor through
`resolve_selector()`. Parsing a command line, and therefore the packet path
that parses one, never reads live entity state, and completion only asks the
source for its online-player name snapshot.

```text
targets          := name | '@' family [ '[' options ']' ]
family           := 'p' | 'a' | 'r' | 's' | 'e' | 'n'
options          := option ( ',' option )* ','?      // whitespace allowed
option           := key '=' value
```

| Family | Selects | Default order | Default limit |
|---|---|---|---|
| `@p` | the nearest player to the origin | nearest | 1 |
| `@a` | every online player | arbitrary | unlimited |
| `@r` | one randomly drawn player | random | 1 |
| `@s` | the entity that runs the command | none | 1 |
| `@e` | every entity the executor can see; this build models players only | arbitrary | unlimited |
| `@n` | the nearest entity; this build models players only | nearest | 1 |

| Filter | Meaning | Notes |
|---|---|---|
| `limit=<n>` | at most `n` results | `n >= 1`, once per selector |
| `sort=nearest\|furthest\|random\|arbitrary` | order before the limit is taken | once per selector |
| `distance=<min>..<max>` | distance range from the origin; either bound may be omitted | must be non-negative, inclusive, once |
| `x=<v>`, `y=<v>`, `z=<v>` | override that component of the origin | once each |
| `dx=<v>`, `dy=<v>`, `dz=<v>` | box filter relative to the origin | once each; see below |
| `name=<name>`, `name=!<name>` | player name filter, exact | one positive entry; any number of inverted ones |
| `gamemode=<mode>`, `gamemode=!<mode>` | game mode filter | names only, case-insensitive; one positive entry |
| `type=<id>`, `type=!<id>` | entity type filter | only `minecraft:player` is modelled; another id fails closed |

An origin is only resolved when a filter or an ordering needs one: `sort=nearest`,
`sort=furthest`, `distance=` and the box filter. `x`/`y`/`z` supply the
components the origin takes; a component that is not given comes from the
command source's feet position. A source without a position (the console) can
therefore still run `@a`, an unfiltered `@e`, a name target, or a selector that
gives all three absolute coordinates, and gets a clear error when it runs `@p`,
`distance=` or the box filter.

The filters the reference defines but this build does not model (`level`,
`x_rotation`, `y_rotation`, `team`, `tag`, `nbt`, `scores`, `advancements` and
`predicate`) are rejected at parse time by name. An unknown option name is
rejected as unknown. A filter is never silently ignored.

The distance filter compares squared distances against the squared bounds, and
its bounds are inclusive. The box filter follows the reference's rule: each
`dx`/`dy`/`dz` extent is widened by one block, a negative extent points the
other way from the origin, and a candidate matches when its hitbox overlaps the
box. The sort is stable, so two players at the same distance keep the
candidate order, which is ascending entity id.

The result of one selector is ordered and then truncated to the limit. A bare
name resolves to at most one player and ignores filters, matching player names
without regard to case. `@s` resolves against the command source entity only: a
source without one matches nothing. `type=` naming any type other than
`minecraft:player` fails closed with an error instead of selecting nothing,
because this build has no other entity type.

`@r` and `sort=random` must not consume a global random generator: their draw
is a deterministic stream seeded from the command source identity, the world
tick and the command text. Repeating the same command at the same tick
therefore makes the same choice, and a replay that reaches the command at that
tick reproduces it; `@r` may pick a different player on a later tick.

Errors follow the reference: a malformed selector is a parse error carrying the
byte position of the offending token, an empty selection is `No entity was
found` (or `No player was found` for a players-only argument), and an argument
form that allows one entity answers `Only one entity is allowed, but the
provided selector allows more than one`.

### Selector limits

The v1 entity model is the player table. `@e` and `@n` select players only, so
`/tp @e ...` moves every player and `/give @e` is rejected because the command's
argument form is players-only. Entity selectors cannot name a player by UUID
yet (a canonical UUID is rejected with an explanatory error), and none of the
state-backed filters above exist. `@e` against a real non-player `type` fails
closed rather than matching the empty set.

`/tp` cannot express the reference's separate one-entity `destination` node
next to `<targets>`, because the dispatcher has no backtracking and a
single-entity sibling registered first would shadow the two-token forms. The
`targets` argument therefore also serves the one-token destination form and
requires exactly one result there, so `/tp @a` answers with the single-entity
error instead of the reference's syntax error.

## Adding a command

Registration attaches literals and arguments to the dispatcher. `then()`
attaches a child, `executes()` attaches an executor, `require_node()` declares
the command's permission node and its vanilla ladder fallback, `require()`
attaches a plain visibility requirement, `tooltip()` supplies the completion
tooltip and `redirect()` continues parsing at another node without consuming
its name.

```cpp
auto gamemode =
    dispatcher.literal("gamemode")
        .require_node("refrain.command.gamemode", command::level::gamemode)
        .tooltip("Change your game mode");
auto mode = dispatcher.argument("mode", command::gamemode_arg());
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
permission filter (see below) is consulted in the same walk. The snapshot owns
its storage, so it stays valid while the dispatcher is mutated. The network
bridge caches one wire tree per subject and per pair of revisions that can
change the answer: the dispatcher revision and the permission policy revision.
A change in either rebuilds the snapshot and resends the tree.

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

- `require_node()` on a node: the stable permission node of the command, for
  example `refrain.command.gamemode` or `refrain.mods.admin`, plus the vanilla
  ladder level used when no permission provider is active. Overloads of
  `require()` remain for a bare level or an arbitrary predicate.
- `dispatcher::set_permission_filter()`: one callback consulted for every node
  that declares a node, in addition to its own requirement. It receives the
  source and the declared node and answers `allowed`, `denied` or
  `not_handled`; `not_handled` falls back to the declared ladder level. The
  composition root installs the check backed by the `refrain:permissions`
  service; the command module itself has no permission registry. The query the
  bridge builds carries the player's world and game mode, so the policy can
  match context-sensitive rules; a node a chain wrapper owns is answered by
  that wrapper, and only the nodes it forwards reach the base resolver.

The same filter decides the tree snapshot, the completion list and the
execution path, so the three cannot disagree about who may use a command. A
denied literal is absent from the tree and from completion, and the network
bridge answers the denial with the node text instead of a generic parse error.
The server console is a `console_source` at the owner level and the permission
service answers it as holding every node.

The `source` interface carries the permission ladder, an opaque identity key and
an optional online-player list used for name completion, so a caller can
implement console, player, command-block or test sources without the network or
world layers. Whether a particular command requires a particular permission is
a registration decision; the reference's permission levels are available as
`command::level`. The complete node table of the built-in tree is in
[Running a server](running.md#in-game-commands).

The built-in command tree also consumes the policy for administration:
`/permissions` declares `refrain.permissions.admin` on every node and its
executors call the host-side management surface of the base resolver. A grant or
revoke changes the policy, persists it and advances the revision, so the tree
snapshot, the completion list and the execution check of every player refresh on
their next tick; the revision a chain reports covers its wrappers as well as the
base policy, so a wrapper change invalidates the same caches. `check` asks the
active provider first, so it reports a node a wrapper decided together with the
answer the base resolver alone would give. The audit record of each change is
described in [Running a server](running.md#permission-administration).

## World and player command batch

The first vanilla command batch registers on the runtime tree and follows the
26.3 argument and bounds rules wherever the modelled world can express them.

| Command | Forms | Behaviour |
|---|---|---|
| `/teleport`, `/tp` | `<x> <y> <z>`, `<target>`, `<targets> <x> <y> <z>`, `<targets> <destination>` | `<target>` and `<targets>` are entity selectors; the source is moved for the one-token destination form and every selected entity for the target-first forms. Moves the source or the selected entities, sends the clientbound position packet and adopts the destination entity's rotation. Integer absolute X/Z name the block centre, `~` is relative to the source and `^` is the source-local basis. The `/tp` literal redirects to `/teleport`, so both share one subtree and one permission check. |
| `/setblock` | `<x> <y> <z> <block> [replace\|keep]` | Parses the canonical block-state key, interns it in the runtime registry and writes one block. `keep` refuses a position that is not air. A position outside the build height or in a column that is not loadable fails before the write. |
| `/fill` | `<from> <to> <block> [replace\|keep\|hollow\|outline]` | Walks the closed region in X, then Y, then Z order and applies one batch. `hollow` fills the shell and clears the interior, `outline` fills the shell only, `keep` leaves non-air positions. The region is rejected above the default `max_block_modifications` bound and when any column is not loadable. |
| `/clone` | `<from> <to> <destination> [replace\|masked]` | Snapshots the source region before writing the destination box, so an overlapping clone is order-independent. `masked` copies non-air blocks only. Same volume and loaded-column checks as `/fill`. |
| `/give` | `<targets> <item> [count]` | Resolves the item in the world item table, caps the count at 100 stacks of the item's stack size and appends to the player's simulation inventory. `<targets>` is a players-only selector, so one item count is given to every selected player. An empty selection or an unknown item fails instead of writing a placeholder. The per-tick inventory sync publishes the changed slots. |
| `/me` | `<action>` | Broadcasts `* <name> <action>` as system chat. |
| `/msg`, `/tell`, `/w` | `<targets> <message>` | Sends the whisper line to every selected player and one confirmation line per target. `/tell` and `/w` redirect to `/msg`. |
| `/save-all` | `[flush]` | Calls the world port's flush, which persists dirty columns and the clock state; both forms perform the same flush. |

Region commands report the reference's bounds failure with the maximum and the
requested volume, reject a region rather than clipping it to the build height,
and publish the applied block changes through the world port's block-edit path,
which the chunk service turns into block-update packets.

### Not covered by this batch

The remaining arguments of these commands need content registries or argument
parsers the tree does not have yet: the rotation and facing forms of `/tp`, `/clone` `filtered`/`force`/`move`/`strict`
and its dimension forms, `/fill` `destroy`/`strict` and the block-predicate
filter, block-state or item components in the argument (the arguments are
words resolved by the executor, and the client tree renders them as strings),
and `/give` overflow as dropped item entities. `/save-all` covers chunks and
the clock; player data, entities and world metadata are not persisted yet.

## Current limits

- Entity selectors resolve against the player table only, so `@e`/`@n` select
  players. The state-backed filters (`tag`, `scores`, `nbt`, `advancements`,
  `predicate`, `team`, `level`, `x_rotation`, `y_rotation`) are rejected
  instead of modelled, a UUID target is not accepted, and a `type=` other
  than `minecraft:player` fails closed. The draw behind `@r` and
  `sort=random` is seeded from the source, the world tick and the command
  text, so it is reproducible but not the reference's global-random stream.
- The reference's registry-backed arguments (block state, block predicate, item
  stack and predicate, nbt, component, style, message, particle, score holder,
  objective, team, slot, function, heightmap, resource or tag lookups, ranges)
  are not registered yet. They need a parser plus a provider for the values and
  are listed in the batches below rather than stubbed. `/setblock`, `/fill` and
  `/clone` therefore take the block state as a word argument and resolve it in
  the executor, and `/give` takes a resource location; the client tree renders
  these as strings and asks the server for completion.
- `/fill` and `/clone` use the reference's default region bound (32768 blocks)
  because the `max_block_modifications` game rule is not modelled, reject a
  region outside the build height instead of clipping it, and require every
  column of the region to be loadable before they write anything.
- `/clone` copies through a snapshot, so an overlapping source and destination
  are order-independent instead of failing the way the reference's normal mode
  does. `/give` appends beyond a full inventory instead of dropping item
  entities, because item entities are not implemented. Chat commands use system
  chat text; the reference's chat types (emote, msg incoming/outgoing) are not
  modelled.
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
| `/clone` | partial | `replace` and `masked` over a single dimension; `filtered`, `force`, `move`, `strict` and the dimension forms are planned for the blocks and entities batch. |
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
| `/fill` | partial | `replace`, `keep`, `hollow` and `outline`; `destroy`, `strict` and the block-predicate filter are planned for the blocks and entities batch. |
| `/fillbiome` | planned | batch 2: blocks and entities |
| `/forceload` | planned | batch 4: execute and functions |
| `/function` | planned | batch 4: execute and functions |
| `/gamemode` | partial | Applies to the calling player only; the reference also accepts a target and has a query form. |
| `/gamerule` | planned | batch 4: execute and functions |
| `/give` | partial | A players-only entity selector target and an item id with a count; item components and NBT are planned for the blocks and entities batch. |
| `/help` | partial | Returns a fixed hint instead of generating usage from the tree. |
| `/item` | planned | batch 2: blocks and entities |
| `/jfr` | planned | batch 5: remaining administration |
| `/kick` | planned | batch 1: player and world basics |
| `/kill` | planned | batch 2: blocks and entities |
| `/list` | implemented | Lists the players currently in play. |
| `/locate` | planned | batch 4: execute and functions |
| `/loot` | planned | batch 2: blocks and entities |
| `/me` | implemented | Broadcasts the emote as system chat; the reference's emote chat type is not modelled. |
| `/msg` | partial | A players-only entity selector target with `/tell` and `/w` aliases; the reference's chat types are not modelled. |
| `/op` | planned | batch 5: remaining administration |
| `/pardon` | planned | batch 5: remaining administration |
| `/pardon-ip` | planned | batch 5: remaining administration |
| `/particle` | planned | batch 2: blocks and entities |
| `/perf` | planned | batch 5: remaining administration |
| `/permissions` | implemented | Server command outside the reference root set: `check`, `grant`, `revoke`, `groups` and `reload` administer the built-in permission policy under the `refrain.permissions.admin` node; `check` reports context rules and chain-wrapper answers, `grant`/`revoke` resolve a bare player name through the live session or, on an offline-mode server, through the deterministic offline profile, and `reload` reloads the base policy and every chain wrapper; the console frontend uses `permissions ...`. See [Running a server](running.md#permission-administration). |
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
| `/save-all` | partial | Flushes dirty chunks and the clock state; player data, entities and level metadata are not persisted yet. |
| `/save-off` | planned | batch 1: player and world basics |
| `/save-on` | planned | batch 1: player and world basics |
| `/say` | implemented | `/say <message>` broadcasts to every player. |
| `/schedule` | planned | batch 4: execute and functions |
| `/scoreboard` | planned | batch 3: scoreboard and teams |
| `/seed` | planned | batch 1: player and world basics |
| `/serverpack` | planned | batch 5: remaining administration |
| `/setblock` | partial | `replace` and `keep`; `destroy`, `strict` and block entity data are planned for the blocks and entities batch. |
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
| `/teleport` | partial | `/tp` alias, position and entity selector destinations, relative and local coordinates; rotation/facing forms and dimension changes are planned for the blocks and entities batch. |
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

Three server commands are not part of the reference's root set: `/mods`
manages the installed mod packages (gated by the `refrain.mods.admin` permission
node), `/permissions` administers the built-in permission policy (gated by
`refrain.permissions.admin`) and `/hardcore` toggles the hardcore flag of the
open world.

