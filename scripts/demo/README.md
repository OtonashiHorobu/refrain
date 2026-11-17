# Demo package assembly

The Windows demonstration package is a staged tree, not a build product of
`scripts/dist.sh` alone: it combines a release archive with a standalone
Minecraft client runtime, a bundled Java runtime and a pre-generated world.
Everything that is specific to that layout lives in the launcher templates
under `launchers/`, so an assembly script does not carry its own copies.

## Launchers

`render_launchers.py` writes `start-client.cmd`, `start-server.cmd`,
`start-demo.cmd` and `stop-server.cmd` into the package root:

```bash
python3 scripts/demo/render_launchers.py --root <staged-package> --uuid <uuid>
```

The UUID is the offline player UUID recorded in the staged client's launch
info. The templates follow the server's default diagnostics layout:

- server log: `logs/refrain.log` under the package root, created by the first
  start;
- crash database: `crash/` under the package root, created by the first start.

`start-server.cmd` prints the server's process exit code and those two paths
after the exe exits, so a clean stop and a crash are distinguishable.
`start-demo.cmd` starts the server in its own console window, waits for
`127.0.0.1:25565` and then starts the client; the server does not inherit the
launcher's stdin.

## Package layout

The staged tree below the package root is fixed by the launchers:

```text
bin/            refrain-server.exe and refrain-tools.exe
gamepack/       composition used by the server
content/        tracked game-derived tables and reports
data/           staged datapack snapshot for offline start
world/          pre-generated world
config/         offline network and permissions configuration
client/         standalone Minecraft client runtime (classpath.txt, jars)
java/           bundled Java runtime (`java/bin/java.exe` is `..\java\bin\java.exe`
                as seen from client/)
start-client.cmd
start-server.cmd
start-demo.cmd
stop-server.cmd
```

The client directory is the current directory of `start-client.cmd`; the
launcher reads the whole classpath from `client/classpath.txt`, one line.
