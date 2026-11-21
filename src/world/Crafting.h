#pragma once
// Crafting (survival): turning resources into other blocks, and making tools. Each
// recipe takes resource units of one or two block types (64 to a block, see
// Block::ResourceUnits) and gives units of another type, or a tool. Tools are made
// once and kept; a survival player can only use the tools they've made (creative
// players have them all). The server checks and applies crafts (Server::HandleCraft);
// the inventory screen shows the recipes.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "world/Armour.h"
#include "world/Block.h"
#include "world/Mining.h"

struct Ingredient {
    BlockType type;
    uint32_t units;
};

struct Recipe {
    const char* id;   // "planks", for commands and tests
    const char* name; // "Planks"
    std::vector<Ingredient> inputs;
    BlockType output = BlockType::AIR; // AIR when it makes a tool
    uint32_t outputUnits = 0;
    Tool tool = Tool::None;
    ArmourPiece armour = ArmourPiece::Count; // a piece of armour (made once, then worn)
    bool ironUpgrade = false;                // the iron upgrade (made once: Crafting::kIronBit)
    // Made once and kept: a tool, a piece of armour, or the upgrade.
    bool MadeOnce() const { return tool != Tool::None || armour != ArmourPiece::Count || ironUpgrade; }
};

namespace Crafting {

constexpr uint32_t kBlock = Block::kUnitsPerBlock;

inline const std::vector<Recipe>& Recipes() {
    static const std::vector<Recipe> recipes = {
        { "planks", "4 planks", { { BlockType::LOG, kBlock } }, BlockType::PLANKS, 4 * kBlock },
        { "stone_brick", "Stone bricks", { { BlockType::STONE, kBlock } }, BlockType::STONE_BRICK, kBlock },
        { "glass", "Glass", { { BlockType::SAND, kBlock } }, BlockType::GLASS, kBlock },
        { "mossy_brick", "Mossy bricks", { { BlockType::STONE_BRICK, kBlock }, { BlockType::LEAVES, kBlock } }, BlockType::MOSSY_BRICK, kBlock },
        { "lamp", "Lamp", { { BlockType::GLASS, kBlock }, { BlockType::PLANKS, kBlock / 2 } }, BlockType::LAMP, kBlock },
        { "pipe", "2 pipes", { { BlockType::STONE, kBlock } }, BlockType::PIPE, 2 * kBlock },
        { "pump", "Pump", { { BlockType::STONE, 2 * kBlock }, { BlockType::GLASS, kBlock } }, BlockType::PUMP, kBlock },
        { "sluice", "Sluice gate", { { BlockType::STONE_BRICK, kBlock }, { BlockType::PLANKS, kBlock } }, BlockType::SLUICE, kBlock },
        { "pressure_plate", "Pressure plate", { { BlockType::STONE, kBlock / 4 } }, BlockType::PRESSURE_PLATE, kBlock },
        { "boat", "Boat", { { BlockType::PLANKS, 3 * kBlock } }, BlockType::BOAT, kBlock },
        { "tnt", "TNT", { { BlockType::SAND, kBlock }, { BlockType::COAL, kBlock / 2 } }, BlockType::TNT, kBlock },
        { "sign", "2 signs", { { BlockType::PLANKS, kBlock } }, BlockType::SIGN, 2 * kBlock },
        { "campfire", "Campfire", { { BlockType::LOG, kBlock }, { BlockType::STONE, kBlock / 4 } }, BlockType::CAMPFIRE, kBlock },
        { "door", "Door", { { BlockType::PLANKS, 2 * kBlock } }, BlockType::DOOR, kBlock },
        { "ladder", "3 ladders", { { BlockType::PLANKS, kBlock } }, BlockType::LADDER, 3 * kBlock },
        { "chest", "Chest", { { BlockType::PLANKS, 2 * kBlock } }, BlockType::CHEST, kBlock },
        { "bed", "Bed", { { BlockType::PLANKS, 3 * kBlock }, { BlockType::HIDE, kBlock } }, BlockType::BED, kBlock },
        { "spikes", "Spikes", { { BlockType::STONE, kBlock } }, BlockType::SPIKES, kBlock },
        { "bread", "Bread", { { BlockType::WHEAT, 3 * kBlock } }, BlockType::BREAD, kBlock },
        { "cooked_fish", "Cooked fish", { { BlockType::RAW_FISH, kBlock }, { BlockType::LOG, kBlock / 4 } }, BlockType::COOKED_FISH, kBlock },
        // Cooked over a little wood.
        { "cooked_venison", "Cooked venison", { { BlockType::RAW_VENISON, kBlock }, { BlockType::LOG, kBlock / 4 } }, BlockType::COOKED_VENISON, kBlock },
        { "pickaxe", "Pickaxe", { { BlockType::PLANKS, kBlock }, { BlockType::STONE, kBlock * 3 / 2 } }, BlockType::AIR, 0, Tool::Pickaxe },
        { "axe", "Axe", { { BlockType::PLANKS, kBlock }, { BlockType::STONE, kBlock } }, BlockType::AIR, 0, Tool::Axe },
        { "shovel", "Shovel", { { BlockType::PLANKS, kBlock }, { BlockType::STONE, kBlock / 2 } }, BlockType::AIR, 0, Tool::Shovel },
        { "sword", "Sword", { { BlockType::PLANKS, kBlock / 2 }, { BlockType::STONE, 2 * kBlock } }, BlockType::AIR, 0, Tool::Sword },
        { "bow", "Bow", { { BlockType::PLANKS, kBlock }, { BlockType::HIDE, kBlock } }, BlockType::AIR, 0, Tool::Bow },
        { "rod", "Fishing rod", { { BlockType::PLANKS, kBlock }, { BlockType::HIDE, kBlock / 4 } }, BlockType::AIR, 0, Tool::Rod },
        { "arrows", "4 arrows", { { BlockType::PLANKS, kBlock / 2 }, { BlockType::STONE, kBlock / 4 } }, BlockType::ARROW, 4 * kBlock },
        // Armour, from deer hide.
        { "cap", "Hide cap", { { BlockType::HIDE, 2 * kBlock } }, BlockType::AIR, 0, Tool::None, ArmourPiece::Cap },
        { "tunic", "Hide tunic", { { BlockType::HIDE, 4 * kBlock } }, BlockType::AIR, 0, Tool::None, ArmourPiece::Tunic },
        { "boots", "Hide boots", { { BlockType::HIDE, 2 * kBlock } }, BlockType::AIR, 0, Tool::None, ArmourPiece::Boots },
        // Smelting, and what iron is for.
        { "iron_ingot", "Iron ingot", { { BlockType::IRON_ORE, kBlock }, { BlockType::COAL, kBlock / 4 } }, BlockType::IRON_INGOT, kBlock },
        // Air power (world/Power.h).
        { "water_wheel", "Water wheel", { { BlockType::PLANKS, 6 * kBlock }, { BlockType::LOG, kBlock } }, BlockType::WATER_WHEEL, kBlock },
        { "shaft", "3 shafts", { { BlockType::LOG, kBlock } }, BlockType::SHAFT, 3 * kBlock },
        { "compressor", "Compressor", { { BlockType::IRON_INGOT, kBlock }, { BlockType::STONE, 2 * kBlock }, }, BlockType::COMPRESSOR, kBlock },
        { "air_tank", "Air tank", { { BlockType::IRON_INGOT, 2 * kBlock }, { BlockType::GLASS, kBlock / 2 } }, BlockType::AIR_TANK, kBlock },
        { "valve", "Valve", { { BlockType::IRON_INGOT, kBlock / 2 }, { BlockType::STONE, kBlock } }, BlockType::VALVE, kBlock },
        // Signals (world/Signals.h).
        { "lever", "2 levers", { { BlockType::PLANKS, kBlock / 4 }, { BlockType::STONE, kBlock / 4 } }, BlockType::LEVER, 2 * kBlock },
        { "logic_gate", "Logic gate", { { BlockType::STONE, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 }, { BlockType::GLASS, kBlock / 4 } }, BlockType::LOGIC_GATE, kBlock },
        { "signal_lamp", "Signal lamp", { { BlockType::GLASS, kBlock }, { BlockType::COAL, kBlock / 4 } }, BlockType::SIGNAL_LAMP, kBlock },
        { "piston", "Piston", { { BlockType::PLANKS, kBlock }, { BlockType::STONE, kBlock }, { BlockType::IRON_INGOT, kBlock / 2 } }, BlockType::PISTON, kBlock },
        { "water_sensor", "Water sensor", { { BlockType::STONE, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 }, { BlockType::SAND, kBlock / 4 } }, BlockType::WATER_SENSOR, kBlock },
        { "pressure_sensor", "Pressure sensor", { { BlockType::STONE, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 }, { BlockType::GLASS, kBlock / 4 } }, BlockType::PRESSURE_SENSOR, kBlock },
        { "daylight_sensor", "Daylight sensor", { { BlockType::PLANKS, kBlock }, { BlockType::GLASS, kBlock / 2 }, { BlockType::IRON_INGOT, kBlock / 4 } }, BlockType::DAYLIGHT_SENSOR, kBlock },
        { "player_sensor", "Player sensor", { { BlockType::STONE, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 }, { BlockType::COAL, kBlock / 4 } }, BlockType::PLAYER_SENSOR, kBlock },
        { "air_pipe", "4 air pipes", { { BlockType::STONE, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 } }, BlockType::AIR_PIPE, 4 * kBlock },
        { "smelter", "Smelter", { { BlockType::STONE_BRICK, 4 * kBlock }, { BlockType::IRON_INGOT, kBlock } }, BlockType::SMELTER, kBlock },
        { "item_pipe", "4 item pipes", { { BlockType::PLANKS, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 } }, BlockType::ITEM_PIPE, 4 * kBlock },
        { "drill", "Drill", { { BlockType::IRON_INGOT, 2 * kBlock }, { BlockType::STONE, 2 * kBlock } }, BlockType::DRILL, kBlock },
        { "coal_lamp", "Lamp (with coal)", { { BlockType::GLASS, kBlock / 2 }, { BlockType::COAL, kBlock / 4 } }, BlockType::LAMP, kBlock },
        { "iron_tools", "Iron upgrade (all tools)", { { BlockType::IRON_INGOT, 3 * kBlock }, { BlockType::PLANKS, kBlock } }, BlockType::AIR, 0, Tool::None,
          ArmourPiece::Count, true },
    };
    return recipes;
}

inline int Find(const char* id) {
    const auto& recipes = Recipes();
    for (size_t i = 0; i < recipes.size(); i++)
        if (std::string(id) == recipes[i].id) return static_cast<int>(i);
    return -1;
}

// The iron upgrade's bit in the tools mask (tools use bits 1..4): every tool you make
// mines at Mining::kIronToolSpeed and hits a heart harder.
constexpr uint8_t kIronBit = 1u << 7;

// Tools held, one bit each (1 << Tool).
inline uint8_t ToolBit(Tool tool) { return static_cast<uint8_t>(1u << static_cast<unsigned>(tool)); }

// How many times `held` (units by block type) covers the recipe's inputs.
template <typename Held>
uint32_t TimesAffordable(const Recipe& recipe, const Held& held) {
    uint32_t times = UINT32_MAX;
    for (const auto& in : recipe.inputs) times = std::min(times, held[static_cast<size_t>(in.type)] / in.units);
    return times;
}

} // namespace Crafting
