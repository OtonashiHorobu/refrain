// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Dumps the light behaviour of every block state of the running target
// version: the block light a state emits, the light levels a path loses when
// it enters the state, and the six face shapes the reference lighting engine
// uses for shape-based light occlusion.
//
// The probe runs against the user's own server jar (the game classes are the
// source of truth); `extract.sh` unpacks the bundler and runs this file in
// source-file mode. The output is a compact JSON document, not a stable
// public format: `pack_report.py` validates it and writes the tracked report.
//
// A face shape is the shape the reference returns for one direction of the
// state's occlusion shape. Light occlusion only uses a state's shape when the
// state both occludes and asks the lighting engine to use its shape; every
// other state has an empty light shape.
//
// Usage: java --class-path <classpath> BlockLightProbe.java <output.json>

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import net.minecraft.SharedConstants;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.VoxelShape;

public final class BlockLightProbe {
    /// Flags of one state; the packed report keeps the same bit values.
    private static final int CAN_OCCLUDE = 1;
    private static final int USE_SHAPE = 2;

    /// The face list order of the report: down, up, north, south, west, east.
    private static final Direction[] FACES = {
        Direction.DOWN, Direction.UP, Direction.NORTH,
        Direction.SOUTH, Direction.WEST, Direction.EAST
    };

    private static String faceKey(VoxelShape shape) {
        StringBuilder key = new StringBuilder();
        for (AABB box : shape.toAabbs()) {
            key.append(Double.toString(box.minX)).append(',')
                .append(Double.toString(box.minY)).append(',')
                .append(Double.toString(box.minZ)).append(',')
                .append(Double.toString(box.maxX)).append(',')
                .append(Double.toString(box.maxY)).append(',')
                .append(Double.toString(box.maxZ)).append(';');
        }
        return key.toString();
    }

    private static void appendFaces(StringBuilder out,
                                    List<List<AABB>> faces) {
        out.append('[');
        for (int face = 0; face < faces.size(); ++face) {
            if (face != 0) {
                out.append(',');
            }
            out.append('[');
            List<AABB> boxes = faces.get(face);
            for (int index = 0; index < boxes.size(); ++index) {
                if (index != 0) {
                    out.append(',');
                }
                AABB box = boxes.get(index);
                double[] bounds = {
                    box.minX, box.minY, box.minZ,
                    box.maxX, box.maxY, box.maxZ
                };
                out.append('[');
                for (int axis = 0; axis < bounds.length; ++axis) {
                    if (axis != 0) {
                        out.append(',');
                    }
                    if (!Double.isFinite(bounds[axis])) {
                        throw new IllegalStateException(
                            "non-finite occlusion shape coordinate");
                    }
                    out.append(Double.toString(bounds[axis]));
                }
                out.append(']');
            }
            out.append(']');
        }
        out.append(']');
    }

    public static void main(String[] args) throws IOException {
        if (args.length != 1) {
            System.err.println("usage: BlockLightProbe.java <output.json>");
            System.exit(2);
        }
        Path output = Path.of(args[0]);

        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        List<BlockState> states = new ArrayList<>();
        for (Block block : BuiltInRegistries.BLOCK) {
            states.addAll(block.getStateDefinition().getPossibleStates());
        }
        states.sort(Comparator.comparingInt(Block::getId));

        StringBuilder light = new StringBuilder();
        StringBuilder shapes = new StringBuilder();
        Map<String, Integer> shapeIds = new HashMap<>();
        int previousId = -1;
        for (BlockState state : states) {
            int id = Block.getId(state);
            if (id <= previousId) {
                throw new IllegalStateException(
                    "state ids are not dense and ascending");
            }
            previousId = id;

            int flags = 0;
            if (state.canOcclude()) {
                flags |= CAN_OCCLUDE;
            }
            if (state.useShapeForLightOcclusion()) {
                flags |= USE_SHAPE;
            }
            int shape = -1;
            if ((flags & (CAN_OCCLUDE | USE_SHAPE)) ==
                (CAN_OCCLUDE | USE_SHAPE)) {
                List<List<AABB>> faces = new ArrayList<>(FACES.length);
                StringBuilder key = new StringBuilder();
                boolean any = false;
                for (Direction face : FACES) {
                    VoxelShape faceShape = state.getFaceOcclusionShape(face);
                    List<AABB> boxes = faceShape.toAabbs();
                    any |= !boxes.isEmpty();
                    faces.add(boxes);
                    key.append(faceKey(faceShape)).append('|');
                }
                if (any) {
                    String signature = key.toString();
                    Integer known = shapeIds.get(signature);
                    if (known == null) {
                        known = shapeIds.size();
                        shapeIds.put(signature, known);
                        if (known != 0) {
                            shapes.append(',');
                        }
                        appendFaces(shapes, faces);
                    }
                    shape = known;
                }
            }

            if (light.length() != 0) {
                light.append(',');
            }
            light.append('[').append(id).append(',')
                 .append(state.getLightEmission()).append(',')
                 .append(state.getLightDampening()).append(',')
                 .append(flags).append(',').append(shape).append(']');
        }

        StringBuilder out = new StringBuilder();
        out.append("{\"block_count\":")
           .append(BuiltInRegistries.BLOCK.size())
           .append(",\"state_count\":").append(states.size())
           .append(",\"shape_count\":").append(shapeIds.size())
           .append(",\"faces\":[\"down\",\"up\",\"north\",\"south\",")
           .append("\"west\",\"east\"]")
           .append(",\"states\":[").append(light).append("],\"shapes\":[")
           .append(shapes).append("]}");
        out.append('\n');
        Files.writeString(output, out.toString(), StandardCharsets.UTF_8);
        System.out.println("block light probe: " + states.size() +
                           " states, " + shapeIds.size() + " occlusion shapes"
                           + " -> " + output);
    }
}
