import net.minecraft.util.Mth;

/** Mth helpers used by the aquifer and surface code. */
public class MthVectors {
    static void dbits(String name, double v) {
        System.out.printf("%s %016x%n", name, Double.doubleToRawLongBits(v));
    }
    static void fbits(String name, float v) {
        System.out.printf("%s %08x%n", name, Float.floatToRawIntBits(v));
    }

    public static void main(String[] args) {
        dbits("map.1", Mth.map(0.3, 0.0, 1.0, -0.8, 0.4));
        dbits("map.2", Mth.map(2.0, 0.0, 1.0, -0.8, 0.4));
        dbits("clampedMap.1", Mth.clampedMap(0.3, 0.0, 64.0, 1.0, 0.0));
        dbits("clampedMap.2", Mth.clampedMap(-5.0, 0.0, 64.0, 1.0, 0.0));
        dbits("clampedMap.3", Mth.clampedMap(70.0, 0.0, 64.0, 1.0, 0.0));
        dbits("inverseLerp", Mth.inverseLerp(3.0, 1.0, 5.0));
        dbits("clampedLerp.1", Mth.clampedLerp(-0.5, 2.0, 6.0));
        dbits("clampedLerp.2", Mth.clampedLerp(1.5, 2.0, 6.0));
        dbits("clampedLerp.3", Mth.clampedLerp(0.25, 2.0, 6.0));
        System.out.printf("quantize.1 %d%n", Mth.quantize(-3.7, 3));
        System.out.printf("quantize.2 %d%n", Mth.quantize(4.9, 3));
        fbits("map.float", Mth.map(0.75F, 0.0F, 1.0F, -1.0F, 3.0F));
        fbits("clampedMap.float",
              Mth.clampedMap(1.5F, 0.0F, 1.0F, -1.0F, 3.0F));
    }
}
