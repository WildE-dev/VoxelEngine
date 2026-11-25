#pragma once
// The item panel's list (Application::DrawItemPanel): every block and item there is to
// carry, in the order the panel shows them. Creative players take stacks from it; in
// survival it's where you look up how a thing is got. The tools (each in its three
// tiers) and the armour follow, from their own enums.
//
// A new block or item type goes here too, unless nobody ever carries it as itself
// (NotCarried below). DataTest checks that every type is in one or the other, so one
// can't be left out by forgetting.
#include "world/Inventory.h"

namespace ItemPalette {

inline const ItemRef kItems[] = {
    // Ground, rock, wood and what grows.
    BlockType::DIRT, BlockType::GRASS, BlockType::DRY_GRASS, BlockType::LUSH_GRASS, BlockType::SNOWY_GRASS, BlockType::STONE, BlockType::DEEPSTONE,
    BlockType::SAND, BlockType::SNOW, BlockType::ICE, BlockType::LOG, BlockType::LEAVES, BlockType::PINE_LEAVES, BlockType::CACTUS, BlockType::LAMP,
    BlockType::STONE_BRICK, BlockType::MOSSY_BRICK, BlockType::CRUMBLING_BRICK,
    // Water, lava and fire.
    BlockType::WATER, BlockType::LAVA, BlockType::OBSIDIAN, BlockType::HAY_BALE, BlockType::FIRE, ItemType::BUCKET, ItemType::WATER_BUCKET,
    BlockType::SPRING, BlockType::SINKHOLE, BlockType::DRAIN, BlockType::PUMP,
    // Air power, fluids and machines.
    BlockType::WATER_WHEEL, BlockType::SHAFT, BlockType::COMPRESSOR, BlockType::AIR_PIPE, BlockType::AIR_TANK, BlockType::CREATIVE_AIR_TANK,
    BlockType::VALVE, BlockType::FLUID_VALVE, BlockType::PRESSURE_GAUGE, BlockType::FLUID_PIPE, BlockType::OBSIDIAN_PIPE, BlockType::FLUID_TANK,
    BlockType::FLUID_OUTLET, BlockType::OIL_DERRICK, BlockType::REFINERY, BlockType::ENGINE, BlockType::BOILER, BlockType::TURBINE,
    BlockType::AIR_DIFFUSER, BlockType::DRILL, BlockType::SMELTER, BlockType::STEEL_FRAME, BlockType::MACHINE_CASING, BlockType::FIRE_BRICK,
    BlockType::ITEM_PIPE, BlockType::FILTER_PIPE, BlockType::EXTRACTOR, BlockType::CRAFTER, BlockType::HARVESTER, BlockType::PISTON,
    BlockType::LAUNCHER,
    // Signals and traps.
    BlockType::LEVER, BlockType::LOGIC_GATE, BlockType::SIGNAL_LAMP, BlockType::WATER_SENSOR, BlockType::PRESSURE_SENSOR,
    BlockType::DAYLIGHT_SENSOR, BlockType::PLAYER_SENSOR, BlockType::HEAT_SENSOR, BlockType::MACHINE_SENSOR, BlockType::SPRINKLER,
    BlockType::SLUICE, BlockType::PRESSURE_PLATE, BlockType::SPIKES,
    // Plants, building blocks, furniture and ores.
    BlockType::TALL_GRASS, BlockType::FLOWER_RED, BlockType::FLOWER_YELLOW, BlockType::DEAD_BUSH, BlockType::PEBBLES, BlockType::PLANKS,
    BlockType::ASPHALT, BlockType::CONCRETE, BlockType::REINFORCED_CONCRETE, BlockType::LIMESTONE, BlockType::GRAVEL, BlockType::GLASS,
    BlockType::CHEST, BlockType::MINING_POST, BlockType::CRUSHER, BlockType::GLOW_FUNGUS, BlockType::MAGMA, BlockType::GOLD_ORE, BlockType::BED,
    BlockType::LADDER, BlockType::DOOR, BlockType::CAMPFIRE, BlockType::SIGN, BlockType::TNT, BlockType::SAPLING, BlockType::PINE_SAPLING,
    BlockType::COAL_ORE, BlockType::IRON_ORE, BlockType::COPPER_ORE, BlockType::OIL_SHALE, BlockType::TAR,
    // Items.
    ItemType::RAW_VENISON, ItemType::COOKED_VENISON, ItemType::HIDE, ItemType::COAL, ItemType::COKE, ItemType::BITUMEN, ItemType::CEMENT,
    ItemType::CRUSHED_IRON, ItemType::IRON_INGOT, ItemType::COPPER_INGOT, ItemType::GOLD_INGOT, ItemType::STEEL_INGOT, ItemType::FORGE_CORE,
    ItemType::HAMMER, ItemType::SEEDS, ItemType::WHEAT, ItemType::BREAD, ItemType::ARROW, ItemType::CROSSBOW, ItemType::RAW_FISH,
    ItemType::COOKED_FISH, ItemType::BOAT,
};

inline constexpr Tool kTools[] = { Tool::Pickaxe, Tool::Axe, Tool::Shovel, Tool::Sword, Tool::Bow, Tool::Rod, Tool::Chisel };

// Blocks nobody carries as themselves, which the panel leaves out: air; the old water
// pipe (a fluid pipe now: see Current); what's grown, not placed (a crop goes in as
// seeds, ripe wheat comes out as wheat); another block in its other state (an open door,
// lit TNT), carried as that block; and what's only part of something bigger.
inline bool NotCarried(BlockType t) {
    switch (t) {
    case BlockType::AIR:
    case BlockType::PIPE:
    case BlockType::CROP:
    case BlockType::RIPE_WHEAT:
    case BlockType::DOOR_OPEN:
    case BlockType::TNT_LIT:
    case BlockType::LEVER_ON:
    case BlockType::SIGNAL_LAMP_ON:
    case BlockType::VALVE_OPEN:
    case BlockType::FLUID_VALVE_OPEN:
    case BlockType::SMELTER_LIT: return true;
    default: return IsPartOnlyType(t);
    }
}

} // namespace ItemPalette
