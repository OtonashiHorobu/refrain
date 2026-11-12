# Client acceptance agent

A `-javaagent` that turns the unmodified 26.3 client into a test bot: it waits
for the join to finish, presses the configured keys, writes a report and halts
the JVM with an exit code the launcher can assert on.

The client jar is obfuscated and the runtime carries no mapping file, so the
agent never reflects into game classes. It reads the client log for the join
sequence and drives the client through `java.awt.Robot`.

## How it works

1. `premain` starts a daemon thread and returns; the client boots normally.
2. The thread tails `logs/latest.log`. `Connecting to` marks the join start;
   a `Loaded <n> advancements` line after it means the level was received and
   the play state was entered.
3. A failure line (`Failed to connect`, `Connection Lost`, `Disconnected`,
   `Internal Exception`) fails the run immediately instead of waiting for the
   timeout.
4. After `settle` seconds the agent presses the configured keys, writes the
   report and halts.

The agent halts with `Runtime.halt` instead of letting the client shut down:
the run is not supposed to save a world.

## Agent arguments

Comma separated `key=value` pairs:

| Key | Meaning |
|---|---|
| `log` | client `logs/latest.log` to watch (required) |
| `report` | JSON report path (required) |
| `server` | server address, copied into the report |
| `timeout` | seconds until the join counts as stalled (default 180) |
| `settle` | seconds to wait after the level loads (default 8) |
| `keys` | `+` separated `KeyEvent` names pressed after the settle delay, for example `E` or `E+ESCAPE` |

After a successful run the agent waits up to 30 seconds for
`<report>.release` so the launcher can take a screenshot of the live window,
then halts with 0. A failed run halts with 1, a bad argument list with 2.

## Running it

`scripts/dev/vanilla-acceptance.sh` builds the agent with the JDK that runs the
client, starts a server on a world, joins it and asserts the result:

```bash
MC_CLIENT_HOME=/path/to/26.3-runtime \
    scripts/dev/vanilla-acceptance.sh --world /path/to/rwf-world
```

The client runtime, the game files and a display all stay outside the
repository, so this harness runs on a workstation and not on hosted CI.

## Limitations

- Input injection needs the game window focused. A Wayland-native window
  ignores synthetic keys from X11 tools, and `Robot` follows the compositor's
  focus; run the client under XWayland and focus its window when the acceptance
  needs key or mouse input.
- The join signal is the client log, not game state: a client that logs the
  level load and then stalls on rendering still counts as joined. The
  screenshot is the evidence for what was actually drawn.
