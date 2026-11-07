# World generation data

Refrain contains no game assets. World generation reads two kinds of
data that come from the Minecraft version the server is built for: the
datapack documents that drive biomes, features, structures and carvers,
and the structure templates that provide the prebuilt pieces.

## Datapack snapshot

The datapack documents are copied into `content/vanilla/<version>/data`
by `tools/worldgen/pack_content.py`, which also writes `MANIFEST.json`
with the game version, data version, entry count and content digest:

```bash
python3 tools/worldgen/pack_content.py \
    --data-root /path/to/extracted/data \
    --out content/vanilla/26.3
```

The snapshot is game-derived data and is covered by the notice in
`content/vanilla/`; it is not part of the repository license.

## Structure templates

Structure templates are binary assets and are not part of the
repository. Extract them from the server jar the operator owns into a
cache directory:

```bash
python3 tools/worldgen/extract_templates.py \
    --server-jar /path/to/server.jar \
    --out ~/.cache/refrain/templates
```

The output uses the layout the template manager expects:

```
<root>/<namespace>/structure/<path>.nbt
```

`TEMPLATES.json` records the game version, template count and content
digest. Extract once per game version; the directory can be regenerated
at any time from the jar.

## Consuming the data

The worldgen layer loads the datapack snapshot from a data root and
templates from the extracted directory. Tests and oracles take both
locations from the environment so the same code paths run against the
files an installation would use.
