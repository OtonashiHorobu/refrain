import net.minecraft.util.RandomSource;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.synth.Noise;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import net.minecraft.world.level.levelgen.synth.PerlinNoise;

/** Prints noise vectors from the real 26.3 classes. */
public class NoiseVectors {
    static final double[][] POINTS_3D = {
        {0.0, 0.0, 0.0},          {1.5, -2.25, 3.75},
        {-100.25, 64.5, 200.125}, {12345.678, -64.5, -9876.543},
        {1.0e8, 1.0e8, 1.0e8},    {-1.0e8, 0.5, 1.0e8},
    };
    static final double[][] POINTS_2D = {
        {0.0, 0.0},
        {1.5, -2.25},
        {-100.25, 200.125},
        {1.0e8, 1.0e8},
    };

    static void fhex(String name, float v) {
        System.out.printf("%s %08x%n", name, Float.floatToRawIntBits(v));
    }

    static void samples(String prefix, Noise noise) {
        for (int i = 0; i < POINTS_3D.length; i++) {
            double[] p = POINTS_3D[i];
            fhex(prefix + ".3d." + i, noise.get(p[0], p[1], p[2]));
        }
        for (int i = 0; i < POINTS_2D.length; i++) {
            double[] p = POINTS_2D[i];
            fhex(prefix + ".2d." + i, noise.get(p[0], p[1]));
        }
        fhex(prefix + ".range",
             noise.range().min()); // symmetric, min is enough
    }

    public static void main(String[] args) {
        // A plain PerlinNoise from a legacy source: tests the permutation and
        // gradient core without the positional-factory chain.
        PerlinNoise perlin = new PerlinNoise(new LegacyRandomSource(42L));
        StringBuilder sb = new StringBuilder();
        perlin.parityConfigString(sb);
        System.out.println("perlin.parity " + sb);
        samples("perlin", perlin);

        // The overworld temperature noise, exactly as RandomState builds it:
        // xorshiro(seed).forkPositional().fromHashOf("minecraft:temperature").
        RandomSource base =
            WorldgenRandom.Algorithm.XOROSHIRO.newInstance(12345L);
        PositionalRandomFactory factory = base.forkPositional();
        RandomSource noiseRandom = factory.fromHashOf("minecraft:temperature");
        double[] temperatureModifiers = {1.5, 0.0, 1.0, 0.0, 0.0, 0.0};
        NormalNoise.Builder temperatureBuilder =
            NormalNoise.builder()
                .setBaseAmplitude(1.2453007926713473)
                .setBaseOctave(-10)
                .setOctaveCount(6);
        for (int i = 0; i < temperatureModifiers.length; i++) {
            temperatureBuilder.setAmplitudeModifier(i, temperatureModifiers[i]);
        }
        NormalNoise temperature = temperatureBuilder.build();
        samples("temperature", temperature.create(noiseRandom));
        fhex("temperature.nn_range", temperature.range().min());

        // A small three-octave noise from a different hash-derived stream.
        RandomSource simpleRandom = factory.fromHashOf("refrain:noise_test");
        NormalNoise simple =
            NormalNoise.builder().setBaseOctave(0).setOctaveCount(3).build();
        samples("simple", simple.create(simpleRandom));
        fhex("simple.nn_range", simple.range().min());

        // Parity construction (legacy-normalised), used by the nether noises.
        RandomSource parityRandom = factory.fromHashOf("refrain:parity_test");
        NormalNoise parity =
            NormalNoise.createParity(-10, 1.5, 0.0, 1.0, 0.0, 0.0, 0.0);
        samples("parity", parity.create(parityRandom));
        fhex("parity.nn_range", parity.range().min());
    }
}
