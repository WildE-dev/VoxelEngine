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
        { "spikes", "Spikes", { { BlockType::STONE, kBlock } }, BlockType::SPIKES, kBlock },
        { "pickaxe", "Pickaxe", { { BlockType::PLANKS, kBlock }, { BlockType::STONE, kBlock * 3 / 2 } }, BlockType::AIR, 0, Tool::Pickaxe },
        { "axe", "Axe", { { BlockType::PLANKS, kBlock }, { BlockType::STONE, kBlock } }, BlockType::AIR, 0, Tool::Axe },
        { "shovel", "Shovel", { { BlockType::PLANKS, kBlock }, { BlockType::STONE, kBlock / 2 } }, BlockType::AIR, 0, Tool::Shovel },
    };
    return recipes;
}

inline int Find(const char* id) {
    const auto& recipes = Recipes();
    for (size_t i = 0; i < recipes.size(); i++)
        if (std::string(id) == recipes[i].id) return static_cast<int>(i);
    return -1;
}

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
