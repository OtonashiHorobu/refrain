import com.mojang.datafixers.util.Pair;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.resources.ResourceKey;

/**
 * Prints a fingerprint of the reference overworld parameter list plus the
 * R-tree selection index for 256 synthetic targets. The fingerprint covers the
 * biome key and every quantized interval, so a wrong table entry is visible
 * even when it never wins a query.
 */
public class BiomeVectors {
    static final int TARGETS = 256;

    static long state = 0x5DEECE66DL;

    static long next() {
        state = state * 6364136223846793005L + 1442695040888963407L;
        return state;
    }

    static long coord() {
        return ((next() >>> 33) % 20001L) - 10000L;
    }

    static long fingerprint(List<Pair<Climate.ParameterPoint, ResourceKey<Biome>>> values) {
        long hash = 0xCBF29CE484222325L;
        for (Pair<Climate.ParameterPoint, ResourceKey<Biome>> value : values) {
            Climate.ParameterPoint point = value.getFirst();
            String line = value.getSecond().identifier() + "|"
                + point.temperature().min() + "," + point.temperature().max() + "|"
                + point.humidity().min() + "," + point.humidity().max() + "|"
                + point.continentalness().min() + "," + point.continentalness().max() + "|"
                + point.erosion().min() + "," + point.erosion().max() + "|"
                + point.depth().min() + "," + point.depth().max() + "|"
                + point.weirdness().min() + "," + point.weirdness().max() + "|"
                + point.offset() + "\n";
            for (int i = 0; i < line.length(); i++) {
                hash ^= line.charAt(i) & 0xFFL;
                hash *= 0x100000001B3L;
            }
        }
        return hash;
    }

    public static void main(String[] args) {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        Map<MultiNoiseBiomeSourceParameterList.Preset, Climate.ParameterList<ResourceKey<Biome>>> presets =
            MultiNoiseBiomeSourceParameterList.knownPresets();
        Climate.ParameterList<ResourceKey<Biome>> overworld =
            presets.get(MultiNoiseBiomeSourceParameterList.Preset.OVERWORLD);

        List<Pair<Climate.ParameterPoint, ResourceKey<Biome>>> values = overworld.values();
        System.out.println("count " + values.size());
        System.out.println("hash " + Long.toHexString(fingerprint(values)));

        List<Pair<Climate.ParameterPoint, Integer>> indexed = new ArrayList<>();
        for (int i = 0; i < values.size(); i++) {
            indexed.add(Pair.of(values.get(i).getFirst(), i));
        }
        Climate.ParameterList<Integer> byIndex = new Climate.ParameterList<>(indexed);

        StringBuilder picks = new StringBuilder("indices ");
        for (int i = 0; i < TARGETS; i++) {
            Climate.TargetPoint target = new Climate.TargetPoint(
                coord(), coord(), coord(), coord(), coord(), coord());
            if (i != 0) {
                picks.append(',');
            }
            picks.append(byIndex.findValue(target));
        }
        System.out.println(picks);
    }
}
