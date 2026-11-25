#pragma once
// Reading what was saved before items had their own types (save format 16 and
// earlier, see server/WorldStorage.h). Then every item was a block type too, so block
// numbers (in chunks, and as ItemIds in inventories, chests and items on the ground)
// counted the items among the blocks: iron ingots were block 39. Each old number is
// looked up by the id it had ("iron_ingot") and becomes what has that id now, a block
// or an item type.
#include <array>
#include <cstdint>

#include "world/Block.h"
#include "world/Inventory.h"

namespace Legacy {

// The block ids in their old order (format 16): never change this list.
constexpr const char* kV16BlockIds[] = {
    "air", "grass", "dirt", "stone", "lamp", "log", "leaves", "tall_grass", "flower_red", "flower_yellow", "sand",
    "water", "pump", "pipe", "stone_brick", "mossy_brick", "spikes", "crumbling_brick", "pressure_plate", "sluice",
    "snow", "dry_grass", "lush_grass", "snowy_grass", "pine_leaves", "cactus", "dead_bush", "ice", "pebbles", "planks",
    "glass", "raw_venison", "cooked_venison", "hide", "chest", "bed", "coal_ore", "iron_ore", "coal", "iron_ingot",
    "seeds", "crop", "ripe_wheat", "wheat", "bread", "ladder", "door", "door_open", "arrow", "raw_fish", "cooked_fish",
    "campfire", "sign", "tnt", "tnt_lit", "boat", "sapling", "pine_sapling", "water_wheel", "air_pipe", "drill",
    "shaft", "compressor", "lever", "lever_on", "logic_gate", "signal_lamp", "signal_lamp_on", "air_tank", "valve",
    "valve_open", "piston", "piston_head", "water_sensor", "pressure_sensor", "daylight_sensor", "player_sensor",
    "item_pipe", "smelter", "smelter_lit", "spring", "steel_ingot", "extractor", "filter_pipe", "crafter", "harvester",
    "drain", "sinkhole", "pressure_gauge", "oil_shale", "tar", "fluid_pipe", "fluid_tank", "oil_derrick", "refinery",
    "engine", "fluid_outlet", "mining_post", "steel_frame", "machine_casing", "fire_brick", "hammer", "multiblock",
    "multiblock_part", "crusher", "crushed_iron", "coke", "glow_fungus", "magma", "gold_ore", "gold_ingot", "crossbow",
    "forge_core", "machine_sensor", "boiler", "turbine", "air_diffuser", "copper_ore", "copper_ingot",
    "creative_air_tank", "lava", "fluid_valve", "fluid_valve_open", "obsidian", "obsidian_pipe", "fire", "bucket",
    "water_bucket", "launcher", "hay_bale", "heat_sensor", "sprinkler", "bitumen", "asphalt", "limestone", "cement",
    "concrete", "gravel"
};
constexpr size_t kV16BlockCount = sizeof kV16BlockIds / sizeof kV16BlockIds[0];

// An old block number as an item now (kNone: no such number).
inline ItemId ItemFromV16Block(uint8_t old) {
    static const std::array<ItemId, 256> table = [] {
        std::array<ItemId, 256> t{};
        for (size_t i = 1; i < kV16BlockCount; i++) {
            const char* id = kV16BlockIds[i];
            const BlockType b = BlockFromId(id);
            const ItemType item = ItemTypeFromId(id);
            t[i] = b != BlockType::COUNT ? Items::Of(b) : item != ItemType::COUNT ? Items::Of(item) : Items::kNone;
        }
        return t;
    }();
    return table[old];
}
// An old block number in a chunk as a block now: COUNT if it isn't one (an item never
// stood in the world, so that's a corrupt chunk).
inline BlockType BlockFromV16(uint8_t old) {
    if (old == 0) return BlockType::AIR;
    const ItemId item = ItemFromV16Block(old);
    return Items::IsBlock(item) ? Items::BlockOf(item) : BlockType::COUNT;
}
// An old ItemId: block numbers are mapped, tools and armour kept.
inline ItemId FromV16(ItemId old) { return old < 256 ? ItemFromV16Block(static_cast<uint8_t>(old)) : old; }
// A filter pipe's old axis (the old block number it let through) as one now.
inline uint8_t FilterAxisFromV16(uint8_t old) { return Items::FilterAxis(ItemFromV16Block(old)); }

} // namespace Legacy
