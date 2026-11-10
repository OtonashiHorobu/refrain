# Developer scripts

## Vanilla 26.3 client

`vanilla-client.sh` launches an unmodified Minecraft Java 26.3 client for
end-to-end tests against a Refrain server. The client runtime is not part of
this repository: no game jar, library or asset is committed.

Provision the runtime outside the tree from the official 26.3 version manifest,
checking every SHA-1 against that manifest and against asset index 34. A
complete runtime looks like this:

```text
<runtime>/
    26.3.json
    client.jar
    libraries/                  # all 114 manifest entries
    assets/indexes/34.json
    assets/objects/<xx>/<sha1>  # 5,147 objects
    logging/client-1.21.2.xml
```

Point `MC_CLIENT_HOME` at that directory. Validate without opening a window:

```bash
scripts/dev/vanilla-client.sh --check
```

The check verifies the client jar, every library, the asset index and all
5,147 asset objects, and requires Java 25 or newer. Only the 66 libraries whose
rules allow Linux are put on the classpath; native jars are extracted under
`MC_NATIVES_DIR`.

Launch with quick play:

```bash
MC_CLIENT_HOME=/path/to/runtime scripts/dev/vanilla-client.sh
MC_SERVER=127.0.0.1:25566 scripts/dev/vanilla-client.sh
```

| Variable | Default | Purpose |
|---|---|---|
| `MC_CLIENT_HOME` | required | runtime root with the version json, jar, libraries and assets |
| `MC_RUN_DIR` | `$MC_CLIENT_HOME/run` | game directory for options, saves, logs and crash reports |
| `MC_SERVER` | `127.0.0.1:25565` | quick-play `host:port`; empty starts at the title screen |
| `MC_NATIVES_DIR` | `$MC_CLIENT_HOME/natives` | LWJGL, JNA and Netty native extraction |
| `MC_JAVA` | `$JAVA_HOME/bin/java`, else `java` | Java 25+ executable |
| `MC_USERNAME` | `Dev` | offline username |

Runtime writes stay below `MC_RUN_DIR` and `MC_NATIVES_DIR`; keep both outside
the repository so no generated game data can be staged or committed.
