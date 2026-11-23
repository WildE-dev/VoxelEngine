#pragma once
// Crafting (survival): turning what you carry into other blocks, tools and armour. Each
// recipe takes units of one or more block types (64 to a block, see
// Block::ResourceUnits; part of a block comes out of the type's offcut, see
// world/Inventory.h) and gives units of another type, a tool (stone) or a piece of
// armour, into the player's inventory. A survival player can only use the tools they
// carry (creative players have them all, of steel). An upgrade recipe makes the tool in
// your hand iron (from stone) or steel (from iron). The server checks and applies crafts
// (Server::HandleCraft); the inventory screen shows the recipes.
// They're data: Resources/Data/recipes.json (kRecipesPath), read at start and again when
// it changes (Load); without it, the game's own (Defaults). The order matters: a
// crafter keeps its recipe by its place in the list. The server and its players
// should share the file.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "world/Armour.h"
#include "world/Block.h"
#include "world/Inventory.h"
#include "world/Mining.h"

struct Ingredient {
    BlockType type;
    uint32_t units;
};

struct Recipe {
    std::string id;   // "planks", for commands and tests
    std::string name; // "Planks"
    std::vector<Ingredient> inputs;
    BlockType output = BlockType::AIR; // AIR when it makes a tool
    uint32_t outputUnits = 0;
    Tool tool = Tool::None;
    ArmourPiece armour = ArmourPiece::Count; // a piece of armour
    bool ironUpgrade = false;                // makes the stone tool in your hand iron
    bool steelUpgrade = false;               // makes the iron tool in your hand steel
    // Makes a tool, a piece of armour or an upgrade, not blocks (crafters can't).
    bool MakesItem() const { return tool != Tool::None || armour != ArmourPiece::Count || ironUpgrade || steelUpgrade; }
    bool Upgrade() const { return ironUpgrade || steelUpgrade; }
};

namespace Crafting {

constexpr uint32_t kBlock = Block::kUnitsPerBlock;

constexpr const char* kRecipesPath = "Resources/Data/recipes.json";

// The game's own recipes.
inline std::vector<Recipe> Defaults() {
    static const std::vector<Recipe> recipes = {
        { "planks", "4 planks", { { BlockType::LOG, kBlock } }, BlockType::PLANKS, 4 * kBlock },
        { "stone_brick", "Stone bricks", { { BlockType::STONE, kBlock } }, BlockType::STONE_BRICK, kBlock },
        { "glass", "Glass", { { BlockType::SAND, kBlock } }, BlockType::GLASS, kBlock },
        { "mossy_brick", "Mossy bricks", { { BlockType::STONE_BRICK, kBlock }, { BlockType::LEAVES, kBlock } }, BlockType::MOSSY_BRICK, kBlock },
        { "lamp", "Lamp", { { BlockType::GLASS, kBlock }, { BlockType::PLANKS, kBlock / 2 } }, BlockType::LAMP, kBlock },
        { "pipe", "2 pipes", { { BlockType::STONE, kBlock } }, BlockType::PIPE, 2 * kBlock },
        { "drain", "Drain", { { BlockType::IRON_INGOT, kBlock / 2 }, { BlockType::STONE, kBlock } }, BlockType::DRAIN, kBlock },
        { "pump", "Pump", { { BlockType::STONE, 2 * kBlock }, { BlockType::GLASS, kBlock } }, BlockType::PUMP, kBlock },
        { "sluice", "Sluice gate", { { BlockType::STONE_BRICK, kBlock }, { BlockType::PLANKS, kBlock } }, BlockType::SLUICE, kBlock },
        { "pressure_plate", "Pressure plate", { { BlockType::STONE, kBlock / 4 }, { BlockType::COPPER_INGOT, kBlock / 8 } }, BlockType::PRESSURE_PLATE, kBlock },
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
        { "chisel", "Chisel", { { BlockType::PLANKS, kBlock / 4 }, { BlockType::STONE, kBlock / 2 } }, BlockType::AIR, 0, Tool::Chisel },
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
        { "pressure_gauge", "Pressure gauge", { { BlockType::GLASS, kBlock / 4 }, { BlockType::IRON_INGOT, kBlock / 4 } }, BlockType::PRESSURE_GAUGE, kBlock },
        { "valve", "Valve", { { BlockType::IRON_INGOT, kBlock / 2 }, { BlockType::STONE, kBlock } }, BlockType::VALVE, kBlock },
        // Signals (world/Signals.h).
        { "lever", "2 levers", { { BlockType::PLANKS, kBlock / 4 }, { BlockType::STONE, kBlock / 4 }, { BlockType::COPPER_INGOT, kBlock / 4 } }, BlockType::LEVER, 2 * kBlock },
        { "logic_gate", "Logic gate", { { BlockType::STONE, kBlock }, { BlockType::COPPER_INGOT, kBlock / 2 }, { BlockType::GLASS, kBlock / 4 } }, BlockType::LOGIC_GATE, kBlock },
        { "signal_lamp", "Signal lamp", { { BlockType::GLASS, kBlock }, { BlockType::COAL, kBlock / 4 }, { BlockType::COPPER_INGOT, kBlock / 4 } }, BlockType::SIGNAL_LAMP, kBlock },
        { "piston", "Piston", { { BlockType::PLANKS, kBlock }, { BlockType::STONE, kBlock }, { BlockType::IRON_INGOT, kBlock / 2 } }, BlockType::PISTON, kBlock },
        { "water_sensor", "Water sensor", { { BlockType::STONE, kBlock }, { BlockType::COPPER_INGOT, kBlock / 4 }, { BlockType::SAND, kBlock / 4 } }, BlockType::WATER_SENSOR, kBlock },
        { "pressure_sensor", "Pressure sensor", { { BlockType::STONE, kBlock }, { BlockType::COPPER_INGOT, kBlock / 4 }, { BlockType::GLASS, kBlock / 4 } }, BlockType::PRESSURE_SENSOR, kBlock },
        { "daylight_sensor", "Daylight sensor", { { BlockType::PLANKS, kBlock }, { BlockType::GLASS, kBlock / 2 }, { BlockType::COPPER_INGOT, kBlock / 4 } }, BlockType::DAYLIGHT_SENSOR, kBlock },
        { "player_sensor", "Player sensor", { { BlockType::STONE, kBlock }, { BlockType::COPPER_INGOT, kBlock / 4 }, { BlockType::COAL, kBlock / 4 } }, BlockType::PLAYER_SENSOR, kBlock },
        { "air_pipe", "4 air pipes", { { BlockType::STONE, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 } }, BlockType::AIR_PIPE, 4 * kBlock },
        { "smelter", "Smelter", { { BlockType::STONE_BRICK, 4 * kBlock }, { BlockType::IRON_INGOT, kBlock } }, BlockType::SMELTER, kBlock },
        { "item_pipe", "4 item pipes", { { BlockType::PLANKS, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 } }, BlockType::ITEM_PIPE, 4 * kBlock },
        { "mining_post", "Mining post", { { BlockType::PLANKS, 4 * kBlock }, { BlockType::IRON_INGOT, kBlock } }, BlockType::MINING_POST, kBlock },
        // Multiblock structures (world/Multiblocks.h).
        { "hammer", "Engineer's hammer", { { BlockType::IRON_INGOT, kBlock }, { BlockType::PLANKS, kBlock / 2 } }, BlockType::HAMMER, kBlock },
        { "steel_frame", "4 steel frames", { { BlockType::STEEL_INGOT, kBlock }, { BlockType::IRON_INGOT, kBlock } }, BlockType::STEEL_FRAME, 4 * kBlock },
        { "machine_casing", "2 machine casings", { { BlockType::STEEL_INGOT, kBlock / 2 }, { BlockType::STONE, 2 * kBlock } }, BlockType::MACHINE_CASING, 2 * kBlock },
        { "fire_brick", "2 fire bricks", { { BlockType::STONE_BRICK, 2 * kBlock }, { BlockType::COAL, kBlock / 4 } }, BlockType::FIRE_BRICK, 2 * kBlock },
        // Oil and fluids (world/Fluids.h).
        { "fluid_pipe", "4 fluid pipes", { { BlockType::IRON_INGOT, kBlock / 4 }, { BlockType::GLASS, kBlock / 4 } }, BlockType::FLUID_PIPE, 4 * kBlock },
        { "fluid_tank", "Fluid tank", { { BlockType::IRON_INGOT, 2 * kBlock }, { BlockType::GLASS, kBlock } }, BlockType::FLUID_TANK, kBlock },
        { "fluid_outlet", "Fluid outlet", { { BlockType::IRON_INGOT, kBlock / 2 }, { BlockType::STONE, kBlock } }, BlockType::FLUID_OUTLET, kBlock },
        { "oil_derrick", "Oil derrick", { { BlockType::STEEL_INGOT, 2 * kBlock }, { BlockType::IRON_INGOT, 2 * kBlock }, { BlockType::PLANKS, 2 * kBlock } }, BlockType::OIL_DERRICK, kBlock },
        { "refinery", "Refinery", { { BlockType::STEEL_INGOT, 2 * kBlock }, { BlockType::STONE_BRICK, 4 * kBlock }, { BlockType::IRON_INGOT, kBlock } }, BlockType::REFINERY, kBlock },
        { "engine", "Engine", { { BlockType::STEEL_INGOT, kBlock }, { BlockType::IRON_INGOT, 2 * kBlock } }, BlockType::ENGINE, kBlock },
        // Automation.
        { "extractor", "Extractor", { { BlockType::IRON_INGOT, kBlock }, { BlockType::PLANKS, kBlock } }, BlockType::EXTRACTOR, kBlock },
        { "filter_pipe", "Filter pipe", { { BlockType::ITEM_PIPE, kBlock }, { BlockType::IRON_INGOT, kBlock / 4 } }, BlockType::FILTER_PIPE, kBlock },
        { "crafter", "Crafter", { { BlockType::STEEL_INGOT, kBlock }, { BlockType::IRON_INGOT, kBlock }, { BlockType::PLANKS, 2 * kBlock } }, BlockType::CRAFTER, kBlock },
        { "harvester", "Harvester", { { BlockType::STEEL_INGOT, kBlock / 2 }, { BlockType::PLANKS, 2 * kBlock } }, BlockType::HARVESTER, kBlock },
        // (Steel comes from a smelter: a drill is the next step after one.)
        { "drill", "Drill", { { BlockType::STEEL_INGOT, kBlock }, { BlockType::IRON_INGOT, kBlock }, { BlockType::STONE, 2 * kBlock } }, BlockType::DRILL, kBlock },
        { "coal_lamp", "Lamp (with coal)", { { BlockType::GLASS, kBlock / 2 }, { BlockType::COAL, kBlock / 4 } }, BlockType::LAMP, kBlock },
        { "iron_tools", "Iron upgrade (tool in hand)", { { BlockType::IRON_INGOT, kBlock } }, BlockType::AIR, 0, Tool::None, ArmourPiece::Count, true },
        { "steel_tools", "Steel upgrade (tool in hand)", { { BlockType::STEEL_INGOT, kBlock } }, BlockType::AIR, 0, Tool::None, ArmourPiece::Count, false, true },
        // (New recipes go at the end: a crafter keeps its recipe by its place here.)
        { "crusher", "Crusher", { { BlockType::STEEL_INGOT, kBlock }, { BlockType::STONE_BRICK, 2 * kBlock } }, BlockType::CRUSHER, kBlock },
        { "shield", "Shield", { { BlockType::PLANKS, 2 * kBlock }, { BlockType::IRON_INGOT, kBlock } }, BlockType::AIR, 0, Tool::None, ArmourPiece::Shield },
        { "plate", "Steel plate", { { BlockType::STEEL_INGOT, 4 * kBlock }, { BlockType::HIDE, 2 * kBlock } }, BlockType::AIR, 0, Tool::None, ArmourPiece::Plate },
        { "crossbow", "Crossbow", { { BlockType::STEEL_INGOT, kBlock }, { BlockType::PLANKS, 2 * kBlock }, { BlockType::HIDE, kBlock } }, BlockType::CROSSBOW, kBlock },
        { "machine_sensor", "Machine sensor", { { BlockType::IRON_INGOT, kBlock / 2 }, { BlockType::COPPER_INGOT, kBlock / 2 }, { BlockType::GLASS, kBlock / 2 } }, BlockType::MACHINE_SENSOR, kBlock },
        { "boiler", "Boiler", { { BlockType::IRON_INGOT, 4 * kBlock }, { BlockType::FIRE_BRICK, 2 * kBlock } }, BlockType::BOILER, kBlock },
        { "turbine", "Steam turbine", { { BlockType::STEEL_INGOT, 3 * kBlock }, { BlockType::FORGE_CORE, kBlock } }, BlockType::TURBINE, kBlock },
        { "air_diffuser", "Air diffuser", { { BlockType::IRON_INGOT, 2 * kBlock }, { BlockType::GLASS, kBlock }, { BlockType::GLOW_FUNGUS, kBlock } }, BlockType::AIR_DIFFUSER, kBlock },
        { "tank", "Oxygen tank", { { BlockType::IRON_INGOT, 3 * kBlock }, { BlockType::GLASS, kBlock } }, BlockType::AIR, 0, Tool::None, ArmourPiece::OxygenTank },
        { "copper_ingot", "Copper ingot", { { BlockType::COPPER_ORE, kBlock }, { BlockType::COAL, kBlock / 4 } }, BlockType::COPPER_INGOT, kBlock },
    };
    return recipes;
}

// The recipes in use. (A reload puts in a new list and leaves the old one be, so the
// server, reading them on its own thread, never finds them gone from under it.)
inline std::atomic<const std::vector<Recipe>*>& Current() {
    static std::atomic<const std::vector<Recipe>*> current{ new std::vector<Recipe>(Defaults()) };
    return current;
}
inline const std::vector<Recipe>& Recipes() { return *Current().load(std::memory_order_acquire); }
// Reads recipes.json (or another file of them): false (with why) and the recipes left as
// they were if it can't be read or has a mistake.
bool Load(const std::string& path, std::string& error);
// Writes the game's own recipes out as a fresh file.
bool WriteDefaults(const std::string& path);

inline int Find(const char* id) {
    const auto& recipes = Recipes();
    for (size_t i = 0; i < recipes.size(); i++)
        if (std::string(id) == recipes[i].id) return static_cast<int>(i);
    return -1;
}

// What tools were before they were items: one bit each in a mask (1 << Tool, skipping
// bit 7 for the tools after the rod), with an iron upgrade (bit 7) and a steel one (bit
// 0) for all of them. Only for reading old saves (WorldStorage).
constexpr uint16_t kOldIronBit = 1u << 7;
constexpr uint16_t kOldSteelBit = 1u << 0;
inline uint16_t OldToolBit(Tool tool) {
    const unsigned t = static_cast<unsigned>(tool);
    return static_cast<uint16_t>(1u << (t < 7 ? t : t + 1));
}

// Units of a type in `held`: an Inventory, or units by type.
inline uint32_t UnitsOf(const Inventory& held, BlockType t) { return held.Units(t); }
template <typename Held>
uint32_t UnitsOf(const Held& held, BlockType t) { return held[static_cast<size_t>(t)]; }

// How many times `held` (units by block type) covers the recipe's inputs.
template <typename Held>
uint32_t TimesAffordable(const Recipe& recipe, const Held& held) {
    uint32_t times = UINT32_MAX;
    for (const auto& in : recipe.inputs) times = std::min(times, UnitsOf(held, in.type) / in.units);
    return times;
}

} // namespace Crafting
