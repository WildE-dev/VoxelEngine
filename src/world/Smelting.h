#pragma once
// What a smelter (world/Power.h) makes from what: each recipe takes resource units of
// one or two types from a chest and puts what it makes back in the same chest. A
// smelter does the first recipe in this list that a chest joined to it can afford.
// Smelting iron ore gives twice what crafting it by hand does, and needs no coal; steel
// (iron and coal) can only be smelted.
#include <cstdint>
#include <vector>

#include "world/Block.h"

struct SmeltRecipe {
    BlockType input;
    uint32_t inputUnits;
    BlockType extra = BlockType::AIR; // a second input (AIR: none)
    uint32_t extraUnits = 0;
    BlockType output;
    uint32_t outputUnits;
};

namespace Smelting {

constexpr uint32_t kBlock = Block::kUnitsPerBlock;

inline const std::vector<SmeltRecipe>& Recipes() {
    static const std::vector<SmeltRecipe> recipes = {
        { BlockType::IRON_ORE, kBlock, BlockType::AIR, 0, BlockType::IRON_INGOT, 2 * kBlock },
        { BlockType::CRUSHED_IRON, kBlock, BlockType::AIR, 0, BlockType::IRON_INGOT, 2 * kBlock }, // (an ore crushed makes two)
        { BlockType::GOLD_ORE, kBlock, BlockType::AIR, 0, BlockType::GOLD_INGOT, kBlock },
        { BlockType::COPPER_ORE, kBlock, BlockType::AIR, 0, BlockType::COPPER_INGOT, 2 * kBlock },
        { BlockType::RAW_VENISON, kBlock, BlockType::AIR, 0, BlockType::COOKED_VENISON, kBlock },
        { BlockType::RAW_FISH, kBlock, BlockType::AIR, 0, BlockType::COOKED_FISH, kBlock },
        { BlockType::SAND, kBlock, BlockType::AIR, 0, BlockType::GLASS, kBlock },
        { BlockType::LOG, kBlock, BlockType::AIR, 0, BlockType::COAL, kBlock / 2 }, // charcoal
        // Last, so ore becomes iron first (and iron you want to keep is best kept in a
        // chest the smelter can't reach).
        // Coke (a coke oven's) makes steel for half the coal's worth.
        { BlockType::IRON_INGOT, kBlock, BlockType::COKE, kBlock / 4, BlockType::STEEL_INGOT, kBlock },
        { BlockType::IRON_INGOT, kBlock, BlockType::COAL, kBlock / 2, BlockType::STEEL_INGOT, kBlock },
    };
    return recipes;
}

// The first recipe these held units afford (nullptr: none).
template <typename Held>
const SmeltRecipe* FirstAffordable(const Held& held) {
    for (const auto& r : Recipes()) {
        if (held[static_cast<size_t>(r.input)] < r.inputUnits) continue;
        if (r.extra != BlockType::AIR && held[static_cast<size_t>(r.extra)] < r.extraUnits) continue;
        return &r;
    }
    return nullptr;
}

} // namespace Smelting
