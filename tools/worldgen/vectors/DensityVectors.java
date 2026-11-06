import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.server.Bootstrap;
import net.minecraft.resources.Identifier;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.levelgen.densityfunction.DensityBuffer;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunctions;
import net.minecraft.world.level.levelgen.densityfunction.DensitySampler;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
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
        DensitySampler sampler = function.compileSampler(CONTEXT);
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
    }
}
