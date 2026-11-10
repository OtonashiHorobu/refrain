import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.XoroshiroRandomSource;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.RandomSupport;
import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.util.Mth;

public class Vectors {
    static void hex(String name, long v) {
        System.out.printf("%s %016x%n", name, v);
    }
    static void dec(String name, long v) {
        System.out.printf("%s %d%n", name, v);
    }
    static void fhex(String name, float v) {
        System.out.printf("%s %08x%n", name, Float.floatToRawIntBits(v));
    }
    static void dhex(String name, double v) {
        System.out.printf("%s %016x%n", name, Double.doubleToRawLongBits(v));
    }

    public static void main(String[] args) {
        // legacy LCG, seed 42
        LegacyRandomSource legacy = new LegacyRandomSource(42L);
        for (int i = 0; i < 6; i++) hex("legacy.next_long." + i, legacy.nextLong());
        LegacyRandomSource l2 = new LegacyRandomSource(42L);
        for (int i = 0; i < 6; i++) dec("legacy.next_int." + i, l2.nextInt());
        LegacyRandomSource l3 = new LegacyRandomSource(42L);
        for (int i = 0; i < 6; i++) dec("legacy.next_int_1000." + i, l3.nextInt(1000));
        LegacyRandomSource l4 = new LegacyRandomSource(42L);
        for (int i = 0; i < 4; i++) fhex("legacy.next_float." + i, l4.nextFloat());
        LegacyRandomSource l5 = new LegacyRandomSource(42L);
        for (int i = 0; i < 4; i++) dhex("legacy.next_double." + i, l5.nextDouble());
        LegacyRandomSource l6 = new LegacyRandomSource(42L);
        dec("legacy.next_boolean.bits", (l6.nextBoolean() ? 1 : 0) | (l6.nextBoolean() ? 2 : 0)
            | (l6.nextBoolean() ? 4 : 0) | (l6.nextBoolean() ? 8 : 0));

        // xoroshiro, seed 42
        XoroshiroRandomSource xo = new XoroshiroRandomSource(42L);
        for (int i = 0; i < 6; i++) hex("xoro.next_long." + i, xo.nextLong());
        XoroshiroRandomSource x2 = new XoroshiroRandomSource(42L);
        for (int i = 0; i < 6; i++) dec("xoro.next_int." + i, x2.nextInt());
        XoroshiroRandomSource x3 = new XoroshiroRandomSource(42L);
        for (int i = 0; i < 6; i++) dec("xoro.next_int_1000." + i, x3.nextInt(1000));
        XoroshiroRandomSource x4 = new XoroshiroRandomSource(42L);
        for (int i = 0; i < 4; i++) fhex("xoro.next_float." + i, x4.nextFloat());
        XoroshiroRandomSource x5 = new XoroshiroRandomSource(42L);
        for (int i = 0; i < 4; i++) dhex("xoro.next_double." + i, x5.nextDouble());

        // seed upgrade and mixing
        RandomSupport.Seed128bit up = RandomSupport.upgradeSeedTo128bit(42L);
        hex("upgrade.lo", up.seedLo());
        hex("upgrade.hi", up.seedHi());
        hex("mix.0", RandomSupport.mixStafford13(0L));
        hex("mix.1", RandomSupport.mixStafford13(1L));
        hex("mix.golden", RandomSupport.mixStafford13(RandomSupport.GOLDEN_RATIO_64));

        RandomSupport.Seed128bit h = RandomSupport.seedFromHashOf("minecraft:aquifer");
        hex("hash.aquifer.lo", h.seedLo());
        hex("hash.aquifer.hi", h.seedHi());
        RandomSupport.Seed128bit h2 = RandomSupport.seedFromHashOf("minecraft:ore");
        hex("hash.ore.lo", h2.seedLo());
        hex("hash.ore.hi", h2.seedHi());

        // Mth.getSeed
        hex("getseed.1_2_3", Mth.getSeed(1, 2, 3));
        hex("getseed.-5_64_7", Mth.getSeed(-5, 64, 7));
        hex("getseed.0_0_0", Mth.getSeed(0, 0, 0));

        // legacy positional factory
        PositionalRandomFactory lf = new LegacyRandomSource(42L).forkPositional();
        dec("legacy.at.1_2_3.next_int", lf.at(1, 2, 3).nextInt());
        dec("legacy.at.1_2_3.next_int_100", lf.at(1, 2, 3).nextInt(100));
        dec("legacy.hash.aquifer.next_int", lf.fromHashOf("minecraft:aquifer").nextInt());

        // xoroshiro positional factory
        PositionalRandomFactory xf = new XoroshiroRandomSource(42L).forkPositional();
        dec("xoro.at.1_2_3.next_int", xf.at(1, 2, 3).nextInt());
        dec("xoro.at.1_2_3.next_int_100", xf.at(1, 2, 3).nextInt(100));
        dec("xoro.at.-5_64_7.next_int", xf.at(-5, 64, 7).nextInt());
        dec("xoro.hash.aquifer.next_int", xf.fromHashOf("minecraft:aquifer").nextInt());

        // worldgen random formulas
        WorldgenRandom w = new WorldgenRandom(new XoroshiroRandomSource(0L));
        hex("decoration.seed", w.setDecorationSeed(12345L, 16, 32));
        dec("decoration.next_int", w.nextInt());
        WorldgenRandom w2 = new WorldgenRandom(new XoroshiroRandomSource(0L));
        w2.setFeatureSeed(777L, 5, 3);
        dec("feature.next_int", w2.nextInt());
        WorldgenRandom w3 = new WorldgenRandom(new XoroshiroRandomSource(0L));
        w3.setLargeFeatureSeed(12345L, -3, 7);
        dec("large.next_int", w3.nextInt());
        WorldgenRandom w4 = new WorldgenRandom(new XoroshiroRandomSource(0L));
        w4.setLargeFeatureWithSalt(12345L, -3, 7, 10387312);
        dec("salt.next_int", w4.nextInt());

        // legacy delegate worldgen random (carvers)
        WorldgenRandom w5 = new WorldgenRandom(new LegacyRandomSource(0L));
        w5.setLargeFeatureSeed(12345L, -3, 7);
        dec("large.legacy.next_int", w5.nextInt());
    }
}
