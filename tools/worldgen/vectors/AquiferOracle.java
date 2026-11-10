import java.util.HashMap;
import java.util.Map;
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunctions;
import net.minecraft.world.level.levelgen.densityfunction.DensitySampler;
import net.minecraft.world.level.levelgen.densityfunction.DensitySamplerSet;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
import net.minecraft.world.level.material.FluidState;

/** Synthetic aquifer decisions: no noise, only gradients/constants. */
public class AquiferOracle {
    static final int[] XS = {0, 8, 15};
    static final int[] ZS = {0, 8, 15};
    static final int[] YS = {-60, -32, 0, 40, 70};
    static final double[] DENSITIES = {-1.0, 0.0, 0.5};

    public static void main(String[] args) {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        DensityFunction.CompileContext ctx = new DensityFunction.CompileContext() {
            public net.minecraft.world.level.levelgen.synth.Noise createNoiseSampler(
                net.minecraft.core.Holder<net.minecraft.world.level.levelgen.synth.NormalNoise> p) {
                throw new UnsupportedOperationException();
            }
            public net.minecraft.util.RandomSource createRandom(net.minecraft.resources.Identifier s) {
                throw new UnsupportedOperationException();
            }
            public net.minecraft.util.RandomSource createEndIslandRandom() {
                throw new UnsupportedOperationException();
            }
        };
        Map<DensityFunction, DensitySampler> compiled = new HashMap<>();
        DensitySamplerSet set = new DensitySamplerSet() {
            @Override
            public DensitySampler.Bound get(DensityFunction function) {
                return compiled.computeIfAbsent(function, f -> f.compileSampler(ctx))
                    .bind(SamplerContext.EMPTY_UNCACHED);
            }
        };

        DensityFunction barrier = DensityFunctions.yClampedGradient(-64, 320, -1.0F, 1.0F);
        DensityFunction flooded = DensityFunctions.yClampedGradient(-64, 320, 1.0F, -1.0F);
        DensityFunction spread = DensityFunctions.yClampedGradient(-64, 320, 0.5F, -0.5F);
        DensityFunction lava = DensityFunctions.constant(0.5F);
        DensityFunction exclusion = DensityFunctions.constant(-1.0F);
        DensityFunction surface = DensityFunctions.constant(30.0F);

        Aquifer.Config config = new Aquifer.Config(barrier, flooded, spread, lava, exclusion, surface);
        PositionalRandomFactory factory =
            WorldgenRandom.Algorithm.XOROSHIRO.newInstance(12345L).forkPositional();
        Aquifer.FluidPicker picker = (x, y, z) -> y < Math.min(-54, 63)
            ? new Aquifer.FluidStatus(-54, Blocks.LAVA.defaultBlockState())
            : new Aquifer.FluidStatus(63, Blocks.WATER.defaultBlockState());
        DensityVolume volume = new DensityVolume(16, 384, 16, 0, -64, 0);
        Aquifer aquifer = config.create(set, factory, volume, picker);

        int index = 0;
        for (int x : XS) {
            for (int z : ZS) {
                for (int y : YS) {
                    for (double density : DENSITIES) {
                        var state = aquifer.computeSubstance(x, y, z, density);
                        int code = state == null ? -1
                            : state.is(Blocks.AIR) ? 0
                            : state.is(Blocks.WATER) ? 2
                            : state.is(Blocks.LAVA) ? 3
                            : 8;
                        System.out.printf("pt.%d %d %d%n", index, code,
                            aquifer.shouldScheduleFluidUpdate() ? 1 : 0);
                        index++;
                    }
                }
            }
        }
    }
}
