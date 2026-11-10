import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.synth.SimplexNoise;

/** Legacy SimplexNoise samples and the frozen-temperature / biome-info noises. */
public class SimplexVectors {
    static void print(String tag, float value) {
        System.out.printf("%s %08x%n", tag, Float.floatToRawIntBits(value));
    }

    static SimplexNoise noise(long seed, boolean discardOffset) {
        RandomSource random = new WorldgenRandom(new LegacyRandomSource(seed));
        return new SimplexNoise(random, discardOffset);
    }

    public static void main(String[] args) {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        SimplexNoise simplex = noise(1234, true);
        double[][] points2d = {{0, 0}, {1.5, -2.25}, {100.25, 33.5}, {-7.75, 64.125}};
        for (int i = 0; i < points2d.length; i++) {
            print("s2." + i, simplex.get(points2d[i][0], points2d[i][1]));
        }
        double[][] points3d = {{0, 0, 0}, {1.5, -2.25, 3.125}, {100.25, 33.5, -7.75}};
        for (int i = 0; i < points3d.length; i++) {
            print("s3." + i, simplex.get(points3d[i][0], points3d[i][1], points3d[i][2]));
        }

        SimplexNoise offset = noise(42, false);
        print("o2", offset.get(1.5, -2.25));
        print("o3", offset.get(1.5, -2.25, 3.125));

        for (int i = 0; i < points3d.length; i++) {
            print("frozen." + i, Biome.FROZEN_TEMPERATURE_NOISE.get(
                points3d[i][0], points3d[i][1], points3d[i][2]));
        }
        double[][] points = {{4, 7}, {-8, 3}};
        for (int i = 0; i < points.length; i++) {
            print("info." + i, Biome.BIOME_INFO_NOISE.get(points[i][0], points[i][1]));
        }
    }
}
