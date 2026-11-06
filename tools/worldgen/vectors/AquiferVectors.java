import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.util.RandomSource;

/** Grid math and cell-center streams of 26.3's noise aquifer. */
public class AquiferVectors {
    static final int[][] CELLS = {
        {0, 0, 0}, {1, -1, 2}, {-3, 5, -7}, {100, -60, -100},
    };

    public static void main(String[] args) {
        RandomSource base = WorldgenRandom.Algorithm.XOROSHIRO.newInstance(12345L);
        PositionalRandomFactory factory = base.forkPositional();
        for (int i = 0; i < CELLS.length; i++) {
            int[] c = CELLS[i];
            RandomSource random = factory.at(c[0], c[1], c[2]);
            int xOffset = random.nextInt(10);
            int yOffset = random.nextInt(9);
            int zOffset = random.nextInt(10);
            System.out.printf("cell.%d.offset %d %d %d%n", i, xOffset, yOffset, zOffset);
            System.out.printf("cell.%d.center %d %d %d%n", i,
                (c[0] << 4) + xOffset, c[1] * 12 + yOffset, (c[2] << 4) + zOffset);
        }
        for (int i = 0; i < 4; i++) {
            int d1 = 3 + i * 7;
            int d2 = d1 + 5 * i + 1;
            System.out.printf("similarity.%d %016x%n", i,
                Double.doubleToRawLongBits(1.0 - (d2 - d1) / 25.0));
        }
    }
}
