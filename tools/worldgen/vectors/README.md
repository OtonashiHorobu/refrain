# World-generation vectors

The probes run the game's own classes and print values the generator depends
on. Their output is the source of truth for the constants frozen in the tests:

| Probe | Test | Covers |
|---|---|---|
| `Vectors.java` | `test/worldgen/test_rng.cpp` | random sources, seed mixing, positional factories, `Mth.getSeed` |
| `NoiseVectors.java` | `test/worldgen/test_noise.cpp` | `PerlinNoise`, `NormalNoise` (builder and parity), `NoiseStack`, bulk `addToVolume` |
| `IntervalVectors.java` | `test/worldgen/test_interval.cpp` | `net.minecraft.util.Interval` operations |
| `DensityVectors.java` | `test/worldgen/test_density.cpp` | density sampler graphs, scalar and bulk paths |
| `MthVectors.java` | `test/worldgen/test_mth.cpp` | `Mth.map`/`clampedMap`/`lerp`/`quantize` |
| `AquiferVectors.java` | `test/worldgen/test_aquifer.cpp` | aquifer grid conversions, cell centers, similarity |

```bash
tools/worldgen/vectors/run.sh /path/to/server.jar [class]
```

The class defaults to `Vectors`; pass `NoiseVectors`, `IntervalVectors` or
`DensityVectors` for the other probes. `DensityVectors` calls
`SharedConstants.tryDetectVersion()` and `Bootstrap.bootStrap()` before using
the density-function registry.

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

`NoiseVectors` covers a plain `PerlinNoise` (legacy source, 42), the overworld
`minecraft:temperature` noise built through
`xorshiro(seed).forkPositional().fromHashOf("minecraft:temperature")`, a small
three-octave noise, and the legacy-normalised `createParity` path. It prints
the raw float bits of six 3D and four 2D samples plus `NormalNoise.range()`.

`nextDouble` is checked by raw bit pattern because the reference implementation
multiplies a 53-bit integer by a `double` constant; the decompiled text shows a
`float` literal for that constant, while the class file holds a `double`. The
probe settles the question from the class file, so the constants must never be
transcribed from decompiled text.

## Updating

After a version bump, run the probe against the new jar, copy the new output
over the constants in the test, and review the diff. A changed vector is a
behavior change in world generation and needs a note in the commit message.
