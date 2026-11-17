// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Dumps the mining behaviour of the running target version: per block the
// destroy time, the correct-tool-for-drops flag, the tool families that mine
// it efficiently and the drop tier it requires; per tool item the tool
// component, the derived family and material, the stack and durability limits
// and the block speeds the component produces.
//
// The probe runs against the user's own server jar (the game classes are the
// source of truth); `extract.sh` unpacks the bundler and runs this file in
// source-file mode. The game's tool components reference block tags, so the
// probe binds the vanilla datapack tags from the jar and builds the item
// components from the same registry before it reads anything. The output is a
// compact JSON document, not a stable public format: `pack_report.py`
// validates it and writes the tracked report.
//
// The report is compressed on purpose: a tiered tool family has one mining
// speed and one drop tier per item, shears and swords carry their answer on
// the block side (a per-block speed and a per-block drop bit), and an item
// with no mining rules has the default speed. The probe proves that this
// compression is exact by re-evaluating getDestroySpeed and
// isCorrectToolForDrops for every tool and every block and comparing the
// result against the calls the game itself makes; a target version whose
// rules no longer fit fails the probe instead of shipping wrong data.
//
// Usage: java --class-path <classpath> MiningProbe.java <output.json>

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.mojang.datafixers.util.Either;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderSet;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.packs.PackResources;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.tags.TagKey;
import net.minecraft.tags.TagLoader;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.Tool;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;

public final class MiningProbe {
    /// Block flags of the report; the packed table keeps the same bit values.
    private static final int FLAG_REQUIRES_CORRECT_TOOL = 1;
    private static final int FLAG_AIR = 2;
    private static final int FLAG_FLUID = 4;

    /// Tool families of the report, in bit order. A block's tool mask and its
    /// direct-correct mask use these bit positions; the sword bit never
    /// appears in the tool mask because no block declares a sword as its
    /// efficient family.
    private static final List<String> TOOL_TYPES =
        List.of("pickaxe", "axe", "shovel", "hoe", "shears", "sword");

    /// Tool families that mine efficiently at the item's single tier speed.
    /// The other families carry their speed on the block side.
    private static final Set<String> TIER_TYPES =
        Set.of("pickaxe", "axe", "shovel", "hoe");

    /// Tool material tag spelling to the material name of the report.
    private static final Map<String, String> MATERIAL_NAMES = Map.of(
        "wooden", "wood",
        "stone", "stone",
        "copper", "copper",
        "iron", "iron",
        "diamond", "diamond",
        "netherite", "netherite",
        "gold", "gold");

    /// Material order of the report. The harvest level of each material is
    /// derived from its incorrect-for tag set, not from this order.
    private static final List<String> MATERIAL_ORDER = List.of(
        "wood", "stone", "copper", "iron", "diamond", "netherite", "gold");

    private static final String MINECRAFT = "minecraft:";

    /// One rule of an item's tool component.
    private static final class Rule {
        String tag;
        List<String> blocks;
        Float speed;
        Boolean correct;
    }

    /// One item that carries a tool component.
    private static final class ToolDump {
        Item item;
        String name;
        String type = "none";
        String material;
        Integer harvestLevel;
        Float tierSpeed;
        float defaultMiningSpeed;
        int damagePerBlock;
        boolean canDestroyBlocksInCreative;
        int maxDamage;
        int maxStackSize;
        final List<Rule> rules = new ArrayList<>();
    }

    /// One block record of the report.
    private static final class BlockDump {
        Block block;
        String name;
        float hardness;
        int flags;
        int toolMask;
        int neededTier;
        float shearsSpeed;
        float swordSpeed;
        int directCorrect;
    }

    private final Map<String, List<String>> tagLists = new HashMap<>();
    private final Map<String, Set<String>> tagSets = new HashMap<>();

    private static String path(String name) {
        return name.startsWith(MINECRAFT)
                   ? name.substring(MINECRAFT.length())
                   : name;
    }

    private static String tagName(TagKey<Block> key) {
        return key.location().toString();
    }

    private static String blockName(Block block) {
        final Identifier key = BuiltInRegistries.BLOCK.getKey(block);
        if (key == null) {
            throw new IllegalStateException("block without a registry name");
        }
        return key.toString();
    }

    private static String itemName(Item item) {
        final Identifier key = BuiltInRegistries.ITEM.getKey(item);
        if (key == null) {
            throw new IllegalStateException("item without a registry name");
        }
        return key.toString();
    }

    private static TagKey<Block> tagKey(String name) {
        return TagKey.create(BuiltInRegistries.BLOCK.key(),
                             Identifier.parse(name));
    }

    private static int bitOf(String type) {
        final int index = TOOL_TYPES.indexOf(type);
        if (index < 0) {
            throw new IllegalStateException("unknown tool family: " + type);
        }
        return 1 << index;
    }

    /// The sorted block names of one bound block tag.
    private List<String> contentsOf(String name) {
        final List<String> known = tagLists.get(name);
        if (known != null) {
            return known;
        }
        final HolderSet.Named<Block> set =
            BuiltInRegistries.BLOCK
                .get(tagKey(name))
                .orElseThrow(() -> new IllegalStateException(
                    "missing block tag: " + name));
        if (!set.isBound()) {
            throw new IllegalStateException("unbound block tag: " + name);
        }
        final Set<String> names = new TreeSet<>();
        for (final Holder<Block> holder : set) {
            names.add(blockName(holder.value()));
        }
        final List<String> sorted = List.copyOf(names);
        tagLists.put(name, sorted);
        tagSets.put(name, names);
        return sorted;
    }

    private Set<String> setOf(String name) {
        contentsOf(name);
        return tagSets.get(name);
    }

    private static boolean matches(Rule rule, String block, MiningProbe probe) {
        if (rule.tag != null) {
            return probe.setOf(rule.tag).contains(block);
        }
        return rule.blocks.contains(block);
    }

    /// Binds the tags of the jar's vanilla datapack to the built-in
    /// registries and then builds the item components against the
    /// bootstrapped data-generator registries, mirroring what a data generator
    /// run does.
    private static void bindGameData() {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        final var pack = ServerPacksSource.createVanillaPackSource();
        try (MultiPackResourceManager manager = new MultiPackResourceManager(
                 PackType.SERVER_DATA,
                 List.<PackResources>of(pack.fullResources()))) {
            final RegistryAccess.Frozen registries =
                RegistryAccess.fromRegistryOfRegistries(
                    BuiltInRegistries.REGISTRY);
            TagLoader.loadTagsForExistingRegistries(manager, registries)
                .forEach(pending -> pending.apply());
        }

        BuiltInRegistries.DATA_COMPONENT_INITIALIZERS
            .build(VanillaRegistries.createWorldLookup())
            .forEach(pending -> pending.apply());
    }

    private static List<Rule> dumpRules(Tool tool) {
        final List<Rule> rules = new ArrayList<>();
        for (final Tool.Rule rule : tool.rules()) {
            final Rule dump = new Rule();
            final Either<TagKey<Block>, List<Holder<Block>>> unwrapped =
                rule.blocks().unwrap();
            final Optional<TagKey<Block>> tag = unwrapped.left();
            if (tag.isPresent()) {
                dump.tag = tagName(tag.get());
            } else {
                final List<String> blocks = new ArrayList<>();
                for (final Holder<Block> holder :
                     unwrapped.right().orElseThrow()) {
                    blocks.add(blockName(holder.value()));
                }
                blocks.sort(String::compareTo);
                dump.blocks = List.copyOf(blocks);
            }
            dump.speed = rule.speed().orElse(null);
            dump.correct = rule.correctForDrops().orElse(null);
            rules.add(dump);
        }
        return rules;
    }

    /// Classifies a tool component into the report's family and material.
    /// Fails instead of guessing when a component does not fit the shapes the
    /// target version uses.
    private static void classify(ToolDump tool) {
        String family = null;
        String material = null;
        boolean shears = false;
        boolean sword = false;
        for (final Rule rule : tool.rules) {
            if (rule.tag == null) {
                continue;
            }
            final String path = path(rule.tag);
            if (rule.correct != null && rule.correct && rule.speed != null &&
                path.startsWith("mineable/")) {
                final String found = path.substring("mineable/".length());
                if (family != null && !family.equals(found)) {
                    throw new IllegalStateException(
                        tool.name + " mines efficiently with two families");
                }
                family = found;
            } else if (rule.correct != null && !rule.correct &&
                       path.startsWith("incorrect_for_") &&
                       path.endsWith("_tool")) {
                final String spelling =
                    path.substring("incorrect_for_".length(),
                                   path.length() - "_tool".length());
                final String found = MATERIAL_NAMES.get(spelling);
                if (found == null) {
                    throw new IllegalStateException(
                        tool.name + " names an unknown tool material: " +
                        spelling);
                }
                material = found;
            } else if (path.startsWith("shears_")) {
                shears = true;
            } else if (path.startsWith("sword_")) {
                sword = true;
            }
        }
        if (family != null) {
            if (!TIER_TYPES.contains(family) || material == null) {
                throw new IllegalStateException(
                    tool.name + " is not a tiered tool");
            }
            tool.type = family;
            tool.material = material;
        } else if (shears) {
            if (material != null) {
                throw new IllegalStateException(
                    tool.name + " is shears and names a material");
            }
            tool.type = "shears";
        } else if (sword) {
            if (material != null) {
                throw new IllegalStateException(
                    tool.name + " is a sword and names a material");
            }
            tool.type = "sword";
        } else if (!tool.rules.isEmpty()) {
            throw new IllegalStateException(
                tool.name + " carries a tool component of unknown shape");
        } else {
            tool.type = "none";
        }
    }

    private List<ToolDump> collectTools() {
        final List<ToolDump> tools = new ArrayList<>();
        for (final Item item : BuiltInRegistries.ITEM) {
            final ItemStack stack = new ItemStack(item);
            final Tool tool = stack.get(DataComponents.TOOL);
            if (tool == null) {
                continue;
            }
            final ToolDump dump = new ToolDump();
            dump.item = item;
            dump.name = itemName(item);
            dump.defaultMiningSpeed = tool.defaultMiningSpeed();
            dump.damagePerBlock = tool.damagePerBlock();
            dump.canDestroyBlocksInCreative =
                tool.canDestroyBlocksInCreative();
            dump.maxDamage = stack.getMaxDamage();
            dump.maxStackSize = stack.getMaxStackSize();
            dump.rules.addAll(dumpRules(tool));
            classify(dump);
            tools.add(dump);
        }
        tools.sort((left, right) -> left.name.compareTo(right.name));
        return tools;
    }

    /// Derives the harvest level of every material from the strict inclusion
    /// order of its incorrect-for tag set: a material that is incorrect for
    /// more blocks sits below the material that is incorrect for fewer. The
    /// level is the number of distinct sets that strictly contain the
    /// material's own set.
    private Map<String, Integer> harvestLevels(List<ToolDump> tools) {
        final Map<String, Set<String>> denied = new LinkedHashMap<>();
        for (final ToolDump tool : tools) {
            if (tool.material == null || denied.containsKey(tool.material)) {
                continue;
            }
            for (final Rule rule : tool.rules) {
                if (rule.tag == null ||
                    !path(rule.tag).startsWith("incorrect_for_")) {
                    continue;
                }
                denied.put(tool.material, setOf(rule.tag));
                break;
            }
        }
        if (!denied.keySet().equals(Set.copyOf(MATERIAL_ORDER))) {
            throw new IllegalStateException(
                "the tool materials of the jar are not the seven of the " +
                "report format: " + denied.keySet());
        }
        final List<Set<String>> distinct = new ArrayList<>();
        for (final Set<String> set : denied.values()) {
            boolean known = false;
            for (final Set<String> existing : distinct) {
                if (existing.equals(set)) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                distinct.add(set);
            }
        }
        // A level only means something when the denial sets are nested.
        for (final Set<String> outer : distinct) {
            for (final Set<String> inner : distinct) {
                if (outer.size() > inner.size() &&
                    !outer.containsAll(inner)) {
                    throw new IllegalStateException(
                        "the incorrect-for tag sets of the tool materials " +
                        "are not nested");
                }
            }
        }
        final Map<String, Integer> levels = new HashMap<>();
        for (final Map.Entry<String, Set<String>> entry : denied.entrySet()) {
            int level = 0;
            for (final Set<String> other : distinct) {
                if (other.size() > entry.getValue().size() &&
                    other.containsAll(entry.getValue())) {
                    ++level;
                }
            }
            levels.put(entry.getKey(), level);
        }
        return levels;
    }

    private void assignHarvestLevels(List<ToolDump> tools,
                                     Map<String, Integer> levels) {
        for (final ToolDump tool : tools) {
            if (tool.material != null) {
                tool.harvestLevel = levels.get(tool.material);
                if (tool.harvestLevel == null) {
                    throw new IllegalStateException(
                        tool.name + " has a material without a level");
                }
            }
            tool.tierSpeed = 1.0F;
            if (TIER_TYPES.contains(tool.type)) {
                for (final Rule rule : tool.rules) {
                    if (rule.speed != null && rule.correct != null &&
                        rule.correct && rule.tag != null &&
                        path(rule.tag).startsWith("mineable/")) {
                        tool.tierSpeed = rule.speed;
                    }
                }
            }
        }
    }

    /// The efficient tag of one tiered family; every item of the family must
    /// name the same tag.
    private Map<String, String> efficientTags(List<ToolDump> tools) {
        final Map<String, String> tags = new HashMap<>();
        for (final ToolDump tool : tools) {
            if (!TIER_TYPES.contains(tool.type)) {
                continue;
            }
            String found = null;
            for (final Rule rule : tool.rules) {
                if (rule.tag != null && rule.speed != null &&
                    rule.correct != null && rule.correct &&
                    path(rule.tag).startsWith("mineable/")) {
                    found = rule.tag;
                }
            }
            if (found == null) {
                throw new IllegalStateException(
                    tool.name + " has no efficient tag");
            }
            final String known = tags.putIfAbsent(tool.type, found);
            if (known != null && !known.equals(found)) {
                throw new IllegalStateException(
                    "two items of family " + tool.type +
                    " name different efficient tags");
            }
        }
        for (final String family : TIER_TYPES) {
            if (!tags.containsKey(family)) {
                throw new IllegalStateException(
                    "no tool item mines efficiently with " + family);
            }
        }
        return tags;
    }

    private static ToolDump representative(List<ToolDump> tools, String type) {
        for (final ToolDump tool : tools) {
            if (tool.type.equals(type)) {
                return tool;
            }
        }
        throw new IllegalStateException("no item of family " + type);
    }

    /// The speed and drop answer the game's own rule loop produces for one
    /// rule list and one block.
    private float ruleSpeed(ToolDump tool, String block) {
        for (final Rule rule : tool.rules) {
            if (rule.speed != null && matches(rule, block, this)) {
                return rule.speed;
            }
        }
        return tool.defaultMiningSpeed;
    }

    private boolean ruleCorrect(ToolDump tool, String block) {
        for (final Rule rule : tool.rules) {
            if (rule.correct != null && matches(rule, block, this)) {
                return rule.correct;
            }
        }
        return false;
    }

    @SuppressWarnings("deprecation")
    private List<BlockDump> collectBlocks(List<ToolDump> tools) {
        final Map<String, String> efficient = efficientTags(tools);
        final Map<String, Integer> materials = harvestLevels(tools);
        assignHarvestLevels(tools, materials);
        final List<String> materialNames = new ArrayList<>(MATERIAL_ORDER);
        int highestLevel = 0;
        for (final int level : materials.values()) {
            highestLevel = Math.max(highestLevel, level);
        }
        final ToolDump shears = representative(tools, "shears");
        final ToolDump sword = representative(tools, "sword");

        final List<BlockDump> blocks = new ArrayList<>();
        int stateCount = 0;
        for (final Block block : BuiltInRegistries.BLOCK) {
            final BlockDump dump = new BlockDump();
            dump.block = block;
            dump.name = blockName(block);
            final BlockState state = block.defaultBlockState();
            dump.hardness = block.defaultDestroyTime();
            if (!(Float.compare(dump.hardness, -1.0F) == 0 ||
                  (Float.isFinite(dump.hardness) && dump.hardness >= 0.0F))) {
                throw new IllegalStateException(
                    dump.name + " has an invalid destroy time: " +
                    dump.hardness);
            }
            final boolean requires = state.requiresCorrectToolForDrops();
            for (final BlockState variant :
                 block.getStateDefinition().getPossibleStates()) {
                ++stateCount;
                if (Float.compare(variant.getDestroySpeed(null, null),
                                  dump.hardness) != 0) {
                    throw new IllegalStateException(
                        dump.name + " has a state with another destroy time");
                }
                if (variant.requiresCorrectToolForDrops() != requires) {
                    throw new IllegalStateException(
                        dump.name +
                        " has a state with another drop requirement");
                }
            }

            dump.flags = (requires ? FLAG_REQUIRES_CORRECT_TOOL : 0) |
                         (state.isAir() ? FLAG_AIR : 0) |
                         (state.liquid() ? FLAG_FLUID : 0);
            dump.toolMask = 0;
            for (final Map.Entry<String, String> entry : efficient.entrySet()) {
                if (setOf(entry.getValue()).contains(dump.name)) {
                    dump.toolMask |= bitOf(entry.getKey());
                }
            }
            dump.neededTier = 0;
            for (final String material : materialNames) {
                if (setOf(incorrectTag(tools, material)).contains(dump.name)) {
                    dump.neededTier =
                        Math.max(dump.neededTier, materials.get(material) + 1);
                }
            }
            if (dump.neededTier > highestLevel) {
                throw new IllegalStateException(
                    dump.name + " needs a tier the report cannot carry");
            }
            final ItemStack shearsStack = new ItemStack(shears.item);
            final ItemStack swordStack = new ItemStack(sword.item);
            final float shearsSpeed = shearsStack.getDestroySpeed(state);
            final float swordSpeed = swordStack.getDestroySpeed(state);
            if (Float.compare(shearsSpeed, ruleSpeed(shears, dump.name)) != 0 ||
                Float.compare(swordSpeed, ruleSpeed(sword, dump.name)) != 0) {
                throw new IllegalStateException(
                    dump.name + " does not reproduce its rule speed");
            }
            dump.shearsSpeed = shearsSpeed;
            dump.swordSpeed = swordSpeed;
            dump.directCorrect = 0;
            if (ruleCorrect(shears, dump.name)) {
                dump.directCorrect |= bitOf("shears");
            }
            if (ruleCorrect(sword, dump.name)) {
                dump.directCorrect |= bitOf("sword");
            }
            blocks.add(dump);
        }
        blocks.sort((left, right) -> left.name.compareTo(right.name));
        System.out.println("mining probe: " + blocks.size() + " block(s), " +
                           stateCount + " state(s)");
        this.stateCount = stateCount;
        this.materials = materials;
        return blocks;
    }

    private int stateCount;

    private Map<String, Integer> materials;

    private static String incorrectTag(List<ToolDump> tools, String material) {
        for (final ToolDump tool : tools) {
            if (!material.equals(tool.material)) {
                continue;
            }
            for (final Rule rule : tool.rules) {
                if (rule.tag != null && rule.correct != null &&
                    !rule.correct &&
                    path(rule.tag).startsWith("incorrect_for_")) {
                    return rule.tag;
                }
            }
        }
        throw new IllegalStateException(
            "no incorrect-for tag for material " + material);
    }

    /// Proves the compressed report against the game itself: for every tool
    /// and every block, the speed and the drop answer derived from the report
    /// must equal the calls the game makes.
    private void verify(List<ToolDump> tools, List<BlockDump> blocks) {
        long checks = 0;
        for (final ToolDump tool : tools) {
            final ItemStack stack = new ItemStack(tool.item);
            for (final BlockDump block : blocks) {
                final BlockState state = block.block.defaultBlockState();
                final float actualSpeed = stack.getDestroySpeed(state);
                final float derivedSpeed = switch (tool.type) {
                    case "pickaxe", "axe", "shovel", "hoe" ->
                        (block.toolMask & bitOf(tool.type)) != 0
                            ? tool.tierSpeed
                            : tool.defaultMiningSpeed;
                    case "shears" -> block.shearsSpeed;
                    case "sword" -> block.swordSpeed;
                    default -> tool.defaultMiningSpeed;
                };
                if (Float.compare(actualSpeed, derivedSpeed) != 0) {
                    throw new IllegalStateException(
                        "speed mismatch for " + tool.name + " on " +
                        block.name + ": game " + actualSpeed + ", report " +
                        derivedSpeed);
                }
                final boolean actualCorrect =
                    stack.isCorrectToolForDrops(state);
                final boolean derivedCorrect = switch (tool.type) {
                    case "pickaxe", "axe", "shovel", "hoe" ->
                        (block.toolMask & bitOf(tool.type)) != 0 &&
                            tool.harvestLevel != null &&
                            tool.harvestLevel >= block.neededTier;
                    case "shears" ->
                        (block.directCorrect & bitOf("shears")) != 0;
                    case "sword" ->
                        (block.directCorrect & bitOf("sword")) != 0;
                    default -> false;
                };
                if (actualCorrect != derivedCorrect) {
                    throw new IllegalStateException(
                        "drop mismatch for " + tool.name + " on " +
                        block.name);
                }
                ++checks;
            }
        }
        System.out.println("mining probe: " + checks +
                           " tool/block pair(s) verified against the game");
    }

    private void write(List<ToolDump> tools, List<BlockDump> blocks,
                       Path output) throws IOException {
        final JsonObject root = new JsonObject();
        root.addProperty("block_count", blocks.size());
        root.addProperty("state_count", stateCount);
        root.addProperty("tool_count", tools.size());
        final JsonArray types = new JsonArray();
        for (final String type : TOOL_TYPES) {
            types.add(type);
        }
        root.add("tool_types", types);
        final JsonArray materialArray = new JsonArray();
        for (final String material : MATERIAL_ORDER) {
            final JsonObject entry = new JsonObject();
            entry.addProperty("name", material);
            entry.addProperty("harvest_level", materials.get(material));
            materialArray.add(entry);
        }
        root.add("materials", materialArray);

        final JsonArray blockArray = new JsonArray();
        for (final BlockDump block : blocks) {
            final JsonArray row = new JsonArray();
            row.add(block.name);
            row.add(block.hardness);
            row.add(block.flags);
            row.add(block.toolMask);
            row.add(block.neededTier);
            row.add(block.shearsSpeed);
            row.add(block.swordSpeed);
            row.add(block.directCorrect);
            blockArray.add(row);
        }
        root.add("blocks", blockArray);

        final JsonArray toolArray = new JsonArray();
        for (final ToolDump tool : tools) {
            final JsonObject entry = new JsonObject();
            entry.addProperty("id", tool.name);
            entry.addProperty("tool_type", tool.type);
            if (tool.material != null) {
                entry.addProperty("material", tool.material);
                entry.addProperty("harvest_level", tool.harvestLevel);
            }
            if (TIER_TYPES.contains(tool.type)) {
                entry.addProperty("tier_speed", tool.tierSpeed);
            }
            entry.addProperty("default_mining_speed",
                              tool.defaultMiningSpeed);
            entry.addProperty("damage_per_block", tool.damagePerBlock);
            entry.addProperty("can_destroy_blocks_in_creative",
                              tool.canDestroyBlocksInCreative);
            entry.addProperty("max_damage", tool.maxDamage);
            entry.addProperty("max_stack_size", tool.maxStackSize);
            final JsonArray rules = new JsonArray();
            for (final Rule rule : tool.rules) {
                final JsonObject ruleObject = new JsonObject();
                if (rule.tag != null) {
                    ruleObject.addProperty("tag", rule.tag);
                } else {
                    final JsonArray names = new JsonArray();
                    for (final String name : rule.blocks) {
                        names.add(name);
                    }
                    ruleObject.add("blocks", names);
                }
                if (rule.speed != null) {
                    ruleObject.addProperty("speed", rule.speed);
                }
                if (rule.correct != null) {
                    ruleObject.addProperty("correct_for_drops", rule.correct);
                }
                rules.add(ruleObject);
            }
            entry.add("rules", rules);
            toolArray.add(entry);
        }
        root.add("tools", toolArray);

        final Map<String, JsonArray> tags = new TreeMap<>();
        for (final ToolDump tool : tools) {
            for (final Rule rule : tool.rules) {
                if (rule.tag == null || tags.containsKey(rule.tag)) {
                    continue;
                }
                final JsonArray names = new JsonArray();
                for (final String name : contentsOf(rule.tag)) {
                    names.add(name);
                }
                tags.put(rule.tag, names);
            }
        }
        final JsonObject tagObject = new JsonObject();
        for (final Map.Entry<String, JsonArray> entry : tags.entrySet()) {
            tagObject.add(entry.getKey(), entry.getValue());
        }
        root.add("tags", tagObject);

        final Gson gson = new GsonBuilder().setPrettyPrinting().create();
        Files.writeString(output, gson.toJson(root) + "\n",
                          StandardCharsets.UTF_8);
        System.out.println("mining probe: wrote " + output);
    }

    private void run(Path output) throws IOException {
        final List<ToolDump> tools = collectTools();
        final List<BlockDump> blocks = collectBlocks(tools);
        verify(tools, blocks);
        write(tools, blocks, output);
    }

    public static void main(String[] args) throws IOException {
        if (args.length != 1) {
            System.err.println("usage: MiningProbe.java <output.json>");
            System.exit(2);
        }
        final Path output = Path.of(args[0]);
        bindGameData();
        new MiningProbe().run(output);
    }
}
