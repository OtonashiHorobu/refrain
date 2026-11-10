import java.lang.reflect.Field;
import java.util.List;
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.levelgen.Beardifier;
import net.minecraft.world.level.levelgen.densityfunction.DensityBuffer;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
import net.minecraft.world.level.levelgen.structure.BoundingBox;
import net.minecraft.world.level.levelgen.structure.TerrainAdjustment;
import net.minecraft.world.level.levelgen.structure.pools.JigsawJunction;
import net.minecraft.world.level.levelgen.structure.pools.StructureTemplatePool;

/** Synthetic Beardifier samples: rigid boxes in every adjustment mode plus junctions. */
public class BeardifierVectors {
    static void printFloat(String tag, float value) {
        System.out.printf("%s %08x%n", tag, Float.floatToRawIntBits(value));
    }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        List<Beardifier.Rigid> rigids = List.of(
            new Beardifier.Rigid(new BoundingBox(0, 60, 0, 15, 70, 15), TerrainAdjustment.BURY, 2),
            new Beardifier.Rigid(new BoundingBox(-10, 50, -10, -1, 60, -1), TerrainAdjustment.BEARD_THIN, 0),
            new Beardifier.Rigid(new BoundingBox(20, 64, 20, 30, 74, 30), TerrainAdjustment.BEARD_BOX, 3),
            new Beardifier.Rigid(new BoundingBox(5, 40, 5, 12, 55, 12), TerrainAdjustment.ENCAPSULATE, 0),
            new Beardifier.Rigid(new BoundingBox(40, 60, 40, 50, 70, 50), TerrainAdjustment.NONE, 0));
        List<JigsawJunction> junctions = List.of(
            new JigsawJunction(8, 64, 8, 0, StructureTemplatePool.Projection.RIGID),
            new JigsawJunction(-8, 55, -8, 0, StructureTemplatePool.Projection.RIGID),
            new JigsawJunction(25, 70, 25, 0, StructureTemplatePool.Projection.RIGID));
        BoundingBox affected = new BoundingBox(0, 40, 0, 50, 74, 50).inflatedBy(24);
        Beardifier beard = new Beardifier(rigids, junctions, affected);

        int[][] points = {
            {8, 64, 8}, {0, 60, 0}, {15, 70, 15}, {-5, 55, -5}, {25, 70, 25},
            {8, 40, 8}, {45, 65, 45}, {-20, 50, -20}, {30, 90, 30}, {12, 0, 12},
        };
        for (int[] point : points) {
            printFloat("p", beard.sampleValue(SamplerContext.EMPTY_UNCACHED,
                                              point[0], point[1], point[2]));
        }

        DensityVolume volume = new DensityVolume(4, 3, 3, -12, 58, -12, 2, 2, 2);
        DensityBuffer buffer = DensityBuffer.createUnpooled(volume.size());
        buffer.fill(0.0F);
        beard.sampleVolume(SamplerContext.EMPTY_UNCACHED, buffer, volume);
        for (int i = 0; i < buffer.size(); i++) {
            System.out.printf("v %d %08x%n", i, Float.floatToRawIntBits(buffer.get(i)));
        }

        Field field = Beardifier.class.getDeclaredField("BEARD_KERNEL");
        field.setAccessible(true);
        float[] kernel = (float[]) field.get(null);
        int[][] entries = {{0, 0, 0}, {12, 12, 12}, {0, 12, 23}, {23, 0, 11}, {5, 7, 9}};
        for (int[] entry : entries) {
            printFloat("k " + entry[0] + " " + entry[1] + " " + entry[2],
                       kernel[entry[2] * 24 * 24 + entry[0] * 24 + entry[1]]);
        }

        printFloat("empty", Beardifier.EMPTY.sampleValue(
            SamplerContext.EMPTY_UNCACHED, 8, 64, 8));
    }
}
