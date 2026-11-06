import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.server.Bootstrap;
import net.minecraft.resources.Identifier;
import net.minecraft.util.CubicSpline;
import net.minecraft.util.Interval;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.levelgen.densityfunction.op.SplineFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensityBuffer;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunctions;
import net.minecraft.world.level.levelgen.densityfunction.DensitySampler;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.densityfunction.generator.NoiseFunction;
import net.minecraft.world.level.levelgen.densityfunction.op.LerpFunction;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import net.minecraft.world.level.levelgen.synth.Noise;

/** Prints scalar and bulk samples for representative DF graphs. */
public class DensityVectors {
    static final DensityFunction.CompileContext CONTEXT = new DensityFunction.CompileContext() {
        @Override
        public Noise createNoiseSampler(Holder<NormalNoise> parameters) {
            throw new UnsupportedOperationException("no noise in these graphs");
        }

        @Override
        public RandomSource createRandom(Identifier seed) {
            throw new UnsupportedOperationException("no random in these graphs");
        }

        @Override
        public RandomSource createEndIslandRandom() {
            throw new UnsupportedOperationException("no end islands here");
        }
    };

    static final int[][] POINTS = {
        {0, -64, 0}, {5, 0, -3}, {-7, 63, 11}, {100, 319, -100},
    };

    static void samples(String name, DensityFunction function) {
        samples(name, function, CONTEXT);
    }

    static void samples(String name, DensityFunction function,
                        DensityFunction.CompileContext context) {
        DensitySampler sampler = function.compileSampler(context);
        for (int i = 0; i < POINTS.length; i++) {
            int[] p = POINTS[i];
            System.out.printf("%s.scalar.%d %08x%n", name, i,
                Float.floatToRawIntBits(
                    sampler.sampleValue(SamplerContext.EMPTY_UNCACHED, p[0], p[1], p[2])));
        }
        DensityVolume volume = new DensityVolume(3, 2, 2, -1, 60, 2, 2, 3, 1);
        DensityBuffer buffer = DensityBuffer.createUnpooled(volume.size());
        buffer.fill(0.0F);
        sampler.sampleVolume(SamplerContext.EMPTY_UNCACHED, buffer, volume);
        for (int i = 0; i < buffer.size(); i++) {
            System.out.printf("%s.volume.%d %08x%n", name, i,
                Float.floatToRawIntBits(buffer.get(i)));
        }
    }

    static void range(String name, DensityFunction function) {
        Interval range = function.range();
        System.out.printf("%s.range %08x %08x%n", name,
            Float.floatToRawIntBits(range.min()),
            Float.floatToRawIntBits(range.max()));
    }

    static void volumeSamples(String name, DensitySampler sampler,
                              DensityVolume volume) {
        DensityBuffer buffer = DensityBuffer.createUnpooled(volume.size());
        buffer.fill(0.0F);
        sampler.sampleVolume(SamplerContext.EMPTY_UNCACHED, buffer, volume);
        for (int i = 0; i < buffer.size(); i++) {
            System.out.printf("%s.%d %08x%n", name, i,
                Float.floatToRawIntBits(buffer.get(i)));
        }
    }

    public static void main(String[] args) {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        DensityFunction gy = DensityFunctions.yClampedGradient(-64, 320, 0.0F, 1.0F);
        DensityFunction gy2 = DensityFunctions.yClampedGradient(0, 128, -2.0F, 2.0F);
        DensityFunction gy3 = DensityFunctions.yClampedGradient(-64, 64, 1.0F, -1.0F);

        samples("add_mul", DensityFunctions.add(
            DensityFunctions.constant(1.5F),
            DensityFunctions.mul(DensityFunctions.constant(2.0F), gy)));
        samples("div_const", DensityFunctions.div(gy2, DensityFunctions.constant(3.0F)));
        samples("min_const", DensityFunctions.min(gy, DensityFunctions.constant(0.25F)));
        samples("max_const", DensityFunctions.max(
            DensityFunctions.sub(DensityFunctions.constant(0.5F), gy2),
            DensityFunctions.constant(-0.2F)));
        samples("squeeze_chain", DensityFunctions.squeeze(DensityFunctions.square(
            DensityFunctions.halfNegative(DensityFunctions.add(gy, DensityFunctions.constant(-0.5F))))));
        samples("clamp_mul", DensityFunctions.clamp(
            DensityFunctions.mul(gy, gy3), -0.5F, 0.5F));
        samples("unary_mix", DensityFunctions.add(
            DensityFunctions.abs(DensityFunctions.quarterNegative(
                DensityFunctions.sub(gy3, DensityFunctions.constant(0.75F)))),
            DensityFunctions.sign(DensityFunctions.mul(gy, DensityFunctions.constant(-1.0F)))));
        samples("cube_sqrt", DensityFunctions.add(
            DensityFunctions.cube(DensityFunctions.constant(0.5F)),
            DensityFunctions.sqrt(DensityFunctions.abs(gy3))));

        NormalNoise.Builder temperatureBuilder = NormalNoise.builder()
            .setBaseAmplitude(1.2453007926713473)
            .setBaseOctave(-10)
            .setOctaveCount(6);
        double[] modifiers = {1.5, 0.0, 1.0, 0.0, 0.0, 0.0};
        for (int i = 0; i < modifiers.length; i++) {
            temperatureBuilder.setAmplitudeModifier(i, modifiers[i]);
        }
        NormalNoise temperature = temperatureBuilder.build();
        NormalNoise simpleParams = NormalNoise.builder()
            .setBaseOctave(0).setOctaveCount(3).build();
        Holder<NormalNoise> temperatureHolder = Holder.direct(temperature);
        Holder<NormalNoise> simpleHolder = Holder.direct(simpleParams);

        RandomSource base = WorldgenRandom.Algorithm.XOROSHIRO.newInstance(12345L);
        PositionalRandomFactory factory = base.forkPositional();
        Noise temperatureNoise = temperature.create(factory.fromHashOf("minecraft:temperature"));
        Noise simpleNoise = simpleParams.create(factory.fromHashOf("refrain:noise_test"));
        DensityFunction.CompileContext noiseContext = new DensityFunction.CompileContext() {
            @Override
            public Noise createNoiseSampler(Holder<NormalNoise> parameters) {
                return parameters.value() == temperature ? temperatureNoise : simpleNoise;
            }

            @Override
            public RandomSource createRandom(Identifier seed) {
                throw new UnsupportedOperationException("no random in these graphs");
            }

            @Override
            public RandomSource createEndIslandRandom() {
                throw new UnsupportedOperationException("no end islands here");
            }
        };

        DensityFunction alphaGy = DensityFunctions.yClampedGradient(-64, 63, 0.0F, 1.0F);
        DensityFunction interpInput = DensityFunctions.add(gy, gy3);
        samples("interp_fn", DensityFunctions.interpolated(interpInput, 4, 8));
        samples("range_const_fn",
            DensityFunctions.rangeChoice(gy, 0.25F, 0.75F,
                DensityFunctions.constant(1.0F), DensityFunctions.constant(2.0F)));
        samples("range_dyn_fn",
            DensityFunctions.rangeChoice(gy, 0.25F, 0.75F, gy2, gy3));
        samples("lerp_const_first_fn", DensityFunctions.lerp(alphaGy, 0.5F, gy2));
        samples("lerp_const_second_fn",
            new LerpFunction(alphaGy, gy2, DensityFunctions.constant(-0.5F)));
        samples("lerp_dyn_fn", DensityFunctions.lerp(alphaGy, gy2, gy3));

        CubicSpline<SplineFunction.Coordinate> basicSpline =
            CubicSpline.builder(new SplineFunction.Coordinate(gy))
                .addPoint(-0.5F, 1.0F, 0.0F)
                .addPoint(0.0F, 0.0F, 2.0F)
                .addPoint(0.5F, 2.0F, -1.0F)
                .build();
        DensityFunction basicFn = new SplineFunction(basicSpline);
        samples("spline_basic_fn", basicFn);
        range("spline_basic_fn", basicFn);

        CubicSpline<SplineFunction.Coordinate> nestedInner =
            CubicSpline.builder(new SplineFunction.Coordinate(gy3))
                .addPoint(-1.0F, 0.25F, 0.5F)
                .addPoint(1.0F, -0.25F, 0.0F)
                .build();
        CubicSpline<SplineFunction.Coordinate> nestedSpline =
            CubicSpline.builder(new SplineFunction.Coordinate(gy))
                .addPoint(-0.25F, 0.5F, 0.0F)
                .addPoint(0.25F, nestedInner)
                .addPoint(0.75F, nestedInner)
                .addPoint(1.25F, 1.5F, 1.0F)
                .build();
        DensityFunction nestedFn = new SplineFunction(nestedSpline);
        samples("spline_nested_fn", nestedFn);
        range("spline_nested_fn", nestedFn);

        DensityFunction constantFn = DensityFunctions.spline(
            CubicSpline.<SplineFunction.Coordinate>constant(0.75F));
        samples("spline_constant_fn", constantFn);
        range("spline_constant_fn", constantFn);

        // Interpolated volume branches: block-step, cell-aligned and block-aligned.
        DensitySampler interpSampler =
            DensityFunctions.interpolated(interpInput, 4, 8).compileSampler(CONTEXT);
        volumeSamples("interp_fn.aligned",
            interpSampler, new DensityVolume(3, 2, 2, -4, 56, 0, 4, 8, 4));
        volumeSamples("interp_fn.block_step",
            interpSampler, new DensityVolume(5, 9, 3, -4, 56, 0));
        volumeSamples("interp_fn.step12",
            interpSampler, new DensityVolume(3, 2, 2, -2, 52, 1, 3, 5, 2));

        samples("noise_fn", DensityFunctions.noise(temperatureHolder, 0.25, 0.5), noiseContext);
        samples("shift_fn", DensityFunctions.shift(temperatureHolder), noiseContext);
        samples("shift_a_fn", DensityFunctions.shiftA(temperatureHolder), noiseContext);
        samples("shift_b_fn", DensityFunctions.shiftB(temperatureHolder), noiseContext);
        DensityFunction shiftFn = DensityFunctions.shift(temperatureHolder);
        samples("shifted_xz_fn",
            DensityFunctions.shiftedNoise2d(shiftFn, shiftFn, 0.25, simpleHolder), noiseContext);
        samples("shifted_xyz_fn",
            new NoiseFunction(simpleHolder, 0.25, 0.5, shiftFn, shiftFn, shiftFn), noiseContext);
    }
}
