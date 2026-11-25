#pragma once
// What a smelter (world/Power.h) makes from what: each recipe takes units of one or two
// block or item types from a chest and puts what it makes back in the same chest. A
// smelter does the first recipe in this list that a chest joined to it can afford.
// Smelting iron ore gives twice what crafting it by hand does, and needs no coal; steel
// (iron and coal) can only be smelted. They're data, like crafting's (world/Crafting.h):
// Resources/Data/smelting.json, in order (the first a chest affords is done).
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "world/Crafting.h"

#include "world/Block.h"

struct SmeltRecipe {
    ItemRef input; // (block or item types)
    uint32_t inputUnits;
    ItemRef extra; // a second input (none: none)
    uint32_t extraUnits = 0;
    ItemRef output;
    uint32_t outputUnits;
};

namespace Smelting {

constexpr uint32_t kBlock = Block::kUnitsPerBlock;

constexpr const char* kPath = "Resources/Data/smelting.json";

// The game's own.
inline std::vector<SmeltRecipe> Defaults() {
    static const std::vector<SmeltRecipe> recipes = {
        { BlockType::IRON_ORE, kBlock, Items::kNone, 0, ItemType::IRON_INGOT, 2 * kBlock },
        { ItemType::CRUSHED_IRON, kBlock, Items::kNone, 0, ItemType::IRON_INGOT, 2 * kBlock }, // (an ore crushed makes two)
        { BlockType::GOLD_ORE, kBlock, Items::kNone, 0, ItemType::GOLD_INGOT, kBlock },
        { BlockType::COPPER_ORE, kBlock, Items::kNone, 0, ItemType::COPPER_INGOT, 2 * kBlock },
        { BlockType::LIMESTONE, kBlock, Items::kNone, 0, ItemType::CEMENT, kBlock }, // (burnt)
        { ItemType::RAW_VENISON, kBlock, Items::kNone, 0, ItemType::COOKED_VENISON, kBlock },
        { ItemType::RAW_FISH, kBlock, Items::kNone, 0, ItemType::COOKED_FISH, kBlock },
        { BlockType::SAND, kBlock, Items::kNone, 0, BlockType::GLASS, kBlock },
        { BlockType::LOG, kBlock, Items::kNone, 0, ItemType::COAL, kBlock / 2 }, // charcoal
        // Last, so ore becomes iron first (and iron you want to keep is best kept in a
        // chest the smelter can't reach).
        // Coke (a coke oven's) makes steel for half the coal's worth.
        { ItemType::IRON_INGOT, kBlock, ItemType::COKE, kBlock / 4, ItemType::STEEL_INGOT, kBlock },
        { ItemType::IRON_INGOT, kBlock, ItemType::COAL, kBlock / 2, ItemType::STEEL_INGOT, kBlock },
    };
    return recipes;
}

// In use (a reload leaves the old list be: see Crafting::Current).
inline std::atomic<const std::vector<SmeltRecipe>*>& Current() {
    static std::atomic<const std::vector<SmeltRecipe>*> current{ new std::vector<SmeltRecipe>(Defaults()) };
    return current;
}
inline const std::vector<SmeltRecipe>& Recipes() { return *Current().load(std::memory_order_acquire); }
bool Load(const std::string& path, std::string& error);
bool WriteDefaults(const std::string& path);

// The first recipe these held units afford (nullptr: none).
template <typename Held>
const SmeltRecipe* FirstAffordable(const Held& held) {
    for (const auto& r : Recipes()) {
        if (Crafting::UnitsOf(held, r.input) < r.inputUnits) continue;
        if (r.extra != Items::kNone && Crafting::UnitsOf(held, r.extra) < r.extraUnits) continue;
        return &r;
    }
    return nullptr;
}

// Does a recipe in `held` (an Inventory): false, and nothing changed, if what it makes
// won't fit.
inline bool Apply(const SmeltRecipe& r, Inventory& held) {
    Inventory after = held;
    if (!after.TakeUnits(r.input, r.inputUnits)) return false;
    if (r.extra != Items::kNone && !after.TakeUnits(r.extra, r.extraUnits)) return false;
    if (after.AddUnits(r.output, r.outputUnits) != 0) return false;
    held = std::move(after);
    return true;
}

} // namespace Smelting
