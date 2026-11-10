import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.BiomeResolver;
import net.minecraft.core.Holder;

/**
 * Prints `BiomeManager.obfuscateSeed` and the quart corner the fuzzy lookup
 * chooses for a fixed set of seeds and block positions. The resolver only
 * records its argument and returns null, so no biome registry is needed.
 */
public class BiomeManagerVectors {
    static final long[] SEEDS = {0L, 12345L};
    static final int[] XS = {-3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    static final int[] YS = {-2, 0, 1};
    static final int[] ZS = {-3, 0, 3, 4};
    static final long[] HASH_SEEDS = {0L, 1L, 12345L, -1L, 987654321L};

    public static void main(String[] args) {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        for (long seed : HASH_SEEDS) {
            System.out.printf("obfuscate %d %016x%n", seed, BiomeManager.obfuscateSeed(seed));
        }

        long hash = 0xCBF29CE484222325L;
        StringBuilder samples = new StringBuilder();
        for (long seed : SEEDS) {
            int[][] recorded = new int[1][3];
            BiomeResolver resolver = (qx, qy, qz) -> {
                recorded[0][0] = qx;
                recorded[0][1] = qy;
                recorded[0][2] = qz;
                return (Holder<Biome>) null;
            };
            BiomeManager manager = new BiomeManager(resolver, BiomeManager.obfuscateSeed(seed));
            for (int x : XS) {
                for (int y : YS) {
                    for (int z : ZS) {
                        manager.getBiome(x, y, z);
                        String line = recorded[0][0] + "," + recorded[0][1] + "," + recorded[0][2] + "\n";
                        for (int i = 0; i < line.length(); i++) {
                            hash ^= line.charAt(i) & 0xFFL;
                            hash *= 0x100000001B3L;
                        }
                        if (samples.length() < 120) {
                            samples.append(recorded[0][0]).append(',')
                                .append(recorded[0][1]).append(',')
                                .append(recorded[0][2]).append(' ');
                        }
                    }
                }
            }
        }
        System.out.printf("fingerprint %016x%n", hash);
        System.out.println("samples " + samples.toString().trim());
    }
}
