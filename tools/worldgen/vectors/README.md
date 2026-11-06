# Random-source vectors

`Vectors.java` runs the game's own random-number classes and prints the draws
that the world generator depends on. Its output is the source of truth for the
constants frozen in `test/worldgen/test_rng.cpp`.

```bash
tools/worldgen/vectors/run.sh /path/to/server.jar
```

The script unpacks the 26.3 bundler jar into a temporary directory, builds the
classpath from `META-INF/classpath-joined`, and runs the probe in source-file
mode. It needs a JDK (26.3 requires Java 25 or newer) and a locally owned
`server.jar`.

## What it covers

| Group | Draws |
|---|---|
| `legacy.next_*` | the 48-bit LCG, bounded and unbounded draws, float/double/boolean |
| `xoro.next_*` | xoroshiro128++, including the Lemire bounded draw |
| `upgrade`, `mix`, `hash` | `RandomSupport` seed mixing and MD5-derived seeds |
| `getseed` | `Mth.getSeed` positional hash |
| `*.at.*`, `*.hash.*` | positional factories, coordinate and name streams |
| `decoration`, `feature`, `large`, `salt` | `WorldgenRandom` seed formulas on both delegates |

`nextDouble` is checked by raw bit pattern because the reference implementation
multiplies a 53-bit integer by a `double` constant; the decompiled text shows a
`float` literal for that constant, while the class file holds a `double`. The
probe settles the question from the class file, so the constants must never be
transcribed from decompiled text.

## Updating

After a version bump, run the probe against the new jar, copy the new output
over the constants in the test, and review the diff. A changed vector is a
behavior change in world generation and needs a note in the commit message.
