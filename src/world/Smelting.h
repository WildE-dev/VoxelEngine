#pragma once
// What a smelter (world/Power.h) makes from what: each recipe takes resource units of
// one or two types from a chest and puts what it makes back in the same chest. A
// smelter does the first recipe in this list that a chest joined to it can afford.
// Smelting iron ore gives twice what crafting it by hand does, and needs no coal.
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
        { BlockType::RAW_VENISON, kBlock, BlockType::AIR, 0, BlockType::COOKED_VENISON, kBlock },
        { BlockType::RAW_FISH, kBlock, BlockType::AIR, 0, BlockType::COOKED_FISH, kBlock },
        { BlockType::SAND, kBlock, BlockType::AIR, 0, BlockType::GLASS, kBlock },
        { BlockType::LOG, kBlock, BlockType::AIR, 0, BlockType::COAL, kBlock / 2 }, // charcoal
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
