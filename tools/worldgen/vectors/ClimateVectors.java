import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.world.level.biome.Climate;

/**
 * Prints the reference `Climate.ParameterList` selections for the synthetic
 * fixture. The fixture holds quantized inputs only, so no game data is
 * involved; the outputs are pasted back into it as the expected values.
 */
public class ClimateVectors {
    static Climate.Parameter interval(JsonArray min, JsonArray max, int d) {
        return new Climate.Parameter(min.get(d).getAsLong(), max.get(d).getAsLong());
    }

    public static void main(String[] args) throws Exception {
        JsonObject root = JsonParser.parseString(Files.readString(Path.of(args[0])))
            .getAsJsonObject();

        List<com.mojang.datafixers.util.Pair<Climate.ParameterPoint, Integer>> values =
            new ArrayList<>();
        int index = 0;
        for (JsonElement element : root.getAsJsonArray("points")) {
            JsonObject point = element.getAsJsonObject();
            JsonArray min = point.getAsJsonArray("min");
            JsonArray max = point.getAsJsonArray("max");
            values.add(com.mojang.datafixers.util.Pair.of(
                new Climate.ParameterPoint(
                    interval(min, max, 0),
                    interval(min, max, 1),
                    interval(min, max, 2),
                    interval(min, max, 3),
                    interval(min, max, 4),
                    interval(min, max, 5),
                    point.get("offset").getAsLong()),
                index++));
        }
        Climate.ParameterList<Integer> list = new Climate.ParameterList<>(values);

        StringBuilder rtree = new StringBuilder("[");
        StringBuilder brute = new StringBuilder("[");
        boolean first = true;
        for (JsonElement element : root.getAsJsonArray("targets")) {
            JsonArray target = element.getAsJsonArray();
            Climate.TargetPoint point = new Climate.TargetPoint(
                target.get(0).getAsLong(),
                target.get(1).getAsLong(),
                target.get(2).getAsLong(),
                target.get(3).getAsLong(),
                target.get(4).getAsLong(),
                target.get(5).getAsLong());
            if (!first) {
                rtree.append(',');
                brute.append(',');
            }
            first = false;
            rtree.append(list.findValue(point));
            brute.append(list.findValueBruteForce(point));
        }
        System.out.println(rtree.append(']'));
        System.out.println(brute.append(']'));
    }
}
