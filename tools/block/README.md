# Block mining data

`tools/block/` extracts the mining data of the target version from a locally
owned server jar and packs it into a tracked report. The report is the source
of the per-block and per-tool tables in `content/vanilla/<version>/tables.rvtb`;
this directory does not change break or place behaviour.

## What the report carries

The report is game-derived data (`reports/block_mining/mining.json` plus
`MANIFEST.json` and `NOTICE.md` under `content/vanilla/<version>/reports/`).

For every block it records:

| Field | Meaning |
|---|---|
| `hardness` | the destroy time the reference caches on every state of the block, with `-1` meaning unbreakable |
| `flags` | whether the block requires a correct tool for drops, and whether it is air or a liquid block |
| `tool_mask` | the tool families that mine the block efficiently, from the `mineable/*` and shears tags |
| `needed_tier` | the lowest tool-material harvest level that yields drops: `0` none, `1` stone/copper, `2` iron, `3` diamond/netherite |
| `shears_speed`, `sword_speed` | the mining speed shears and swords get on the block |
| `direct_correct` | whether shears or a sword are correct for drops on the block |

For every item that carries a `minecraft:tool` component it records the item
id, the tool family, the material and harvest level, the tier speed, the
default mining speed, the damage per block, the creative-mode flag, the stack
limit, the durability and the ordered rules of the component.

The report also carries the block tags its rules reference, so the packed
values can be re-derived and checked without a jar.

## Why the report is compressed

The tool component of the target version is an ordered list of rules, each
binding a block tag or a direct block set to an optional mining speed and an
optional drop answer. The report compresses that model into one speed and one
harvest threshold per tiered family, and a per-block speed and drop bit for
shears and swords. The compression is not assumed: the probe re-evaluates the
game's own `getDestroySpeed` and `isCorrectToolForDrops` for every tool and
every block and aborts if the compressed answer differs from the game. A
future version whose rules no longer fit this shape fails the probe instead of
shipping wrong data.

## Regenerating

Generate the data generator reports once from the jar you own:

```bash
java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --reports
```

Then extract and pack the block mining report next to the other reports:

```bash
tools/block/extract.sh /path/to/server.jar generated/reports
```

The script unpacks the bundler jar, runs `tools/block/MiningProbe.java`
against the game's block registry, item registry and vanilla datapack tags and
packs the result with `tools/block/pack_report.py`. The packer validates the
probe output against `blocks.json` and `minecraft/components/item/`, re-derives
every packed field from the tool rules and block tags, and writes the tracked
report with the digest of the server archive.

Copy the report into the content snapshot and import the tables:

```bash
cp -r generated/reports/block_mining content/vanilla/26.3/reports/
build/Release/bin/refrain-tools vanilla import \
    -r generated/reports -o content/vanilla/26.3 --force
```

Verify a tracked report without a jar:

```bash
python3 tools/block/pack_report.py \
    --check content/vanilla/26.3/reports/block_mining
```

The negative tests of the packer run without a jar either:

```bash
python3 tools/block/test_pack_report.py
```
