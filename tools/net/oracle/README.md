# Network oracle (local only)

This directory holds an opt-in comparison harness for the clientbound protocol
stream. Nothing here downloads, bundles or commits Mojang code, assets or
captures; it requires a locally owned vanilla 26.3 server jar.

## Why

A client joining successfully only proves that the packets it happened to need
were accepted. The oracle requires field-level parity for login, configuration,
registries, tags and a chunk. World generation follows the same pattern: an
external oracle generates reference data, the repository keeps only the
comparison tooling.

## Workflow

```bash
# 1. Capture a locally owned vanilla server (accept its EULA yourself).
python3 tools/net/oracle/run_vanilla.py \
    --server-jar /path/to/server-26.3.jar \
    --libraries /path/to/vanilla/libraries \
    --accept-eula \
    --port 25566 \
    --out /tmp/vanilla.rfcap

# 2. Start Refrain with the same view distance and capture the same path.
./build/Release/bin/refrain-tools protocol capture \
    --host 127.0.0.1:25565 --out /tmp/refrain.rfcap \
    --username OracleBot --wait-chunks 1 --linger-ms 500

# 3. Compare packet sequence and (optionally) payload bytes.
python3 tools/net/oracle/compare.py \
    --states 2 --limit 20 --payload /tmp/vanilla.rfcap /tmp/refrain.rfcap
```

`run_vanilla.py` supports both self-contained server jars (`-jar`) and the
Mojang-style split layout (`--libraries` adds every library jar to the
classpath and launches `net.minecraft.server.Main`). Pass `--world` to copy an
existing Anvil world into the oracle directory so both captures cover the same
chunks.

`compare.py` reports the first state/id or payload mismatch and unique packet
id sets. `--chunks` decodes every `level_chunk_with_light` body and compares
section block/biome values, heightmaps, block entities and light data; it is
the field-level check used for the chunk encoder. Chunks present on only one
side are reported but skipped; pass `--chunk-strict` to fail on them. `--registries` compares the 32 registry headers; `--registry-entries`
skips each entry NBT and compares the registry entry ids and order. `--states`, `--limit` and `--ignore state:id,...` narrow the comparison
to the deterministic login prefix while world-content packets are normalized
separately. Payload comparison is intentionally raw: timestamps, UUIDs, player
counts and registry cache ids are expected to differ until a normalizer is
added for that packet.

## CI

The scripts are not run in CI: a vanilla jar is not available and captures are
not repository content. CI keeps the synthetic protocol tests, schema drift
check, C ABI check and the local bot harness.
