#pragma once
// Mining (survival): breaking a block takes time, holding the button on it. Each
// type has a hardness, the seconds a full block takes by hand; the right tool (a
// pickaxe for stone and bricks, an axe for wood, a shovel for earth) is kToolSpeed
// times faster. A partly carved block takes less, by its volume. Plants break at
// once. In creative everything breaks at once. The server times each break
// (Server::HandleBreak), so a client can't skip it; the client uses the same rules
// to show progress.
#include <algorithm>
#include <cstdint>
#include <string>

#include "world/Block.h"
#include "world/BlockData.h"

// The sword is for fighting (see Combat in world/Creatures.h); it's quick on leaves too.
enum class Tool : uint8_t { None, Pickaxe, Axe, Shovel, Sword, Bow, Rod, Count }; // (the bow shoots arrows, the rod fishes: right-click)

namespace Mining {

constexpr float kToolSpeed = 4.0f;
constexpr float kIronToolSpeed = 6.0f; // with the iron upgrade (see world/Crafting.h)
constexpr float kSteelToolSpeed = 9.0f; // with the steel upgrade
// A carved block takes time by its volume, but no less than this share of a full one.
constexpr float kMinVolumeShare = 0.3f;
// The server lets a break through this much early (network jitter), in seconds plus
// a share of the time it takes.
constexpr double kSlackSeconds = 0.2, kSlackShare = 0.1;

inline const char* ToolName(Tool t) {
    switch (t) {
    case Tool::Pickaxe: return "Pickaxe";
    case Tool::Axe: return "Axe";
    case Tool::Shovel: return "Shovel";
    case Tool::Sword: return "Sword";
    case Tool::Bow: return "Bow";
    case Tool::Rod: return "Fishing rod";
    default: return "Hand";
    }
}
inline const char* ToolId(Tool t) {
    switch (t) {
    case Tool::Pickaxe: return "pickaxe";
    case Tool::Axe: return "axe";
    case Tool::Shovel: return "shovel";
    case Tool::Sword: return "sword";
    case Tool::Bow: return "bow";
    case Tool::Rod: return "rod";
    default: return "hand";
    }
}
// The tool's icon (a texture in Resources/Textures/Blocks).
inline const char* ToolTexture(Tool t) {
    switch (t) {
    case Tool::Pickaxe: return "tool_pickaxe";
    case Tool::Axe: return "tool_axe";
    case Tool::Shovel: return "tool_shovel";
    case Tool::Sword: return "tool_sword";
    case Tool::Bow: return "tool_bow";
    case Tool::Rod: return "tool_rod";
    default: return nullptr;
    }
}

// Seconds a full block of this type takes by hand (0: at once): the game's own (see
// Hardness, below, for what's used).
inline float DefaultHardness(BlockType type) {
    switch (type) {
    case BlockType::AIR:
    case BlockType::WATER:
    case BlockType::TALL_GRASS:
    case BlockType::FLOWER_RED:
    case BlockType::FLOWER_YELLOW:
    case BlockType::DEAD_BUSH:
    case BlockType::CROP:
    case BlockType::RIPE_WHEAT:
    case BlockType::SAPLING:
    case BlockType::PINE_SAPLING:
    case BlockType::PEBBLES: return 0.0f;
    case BlockType::LEAVES:
    case BlockType::PINE_LEAVES: return 0.3f;
    case BlockType::DIRT:
    case BlockType::GRASS:
    case BlockType::DRY_GRASS:
    case BlockType::LUSH_GRASS:
    case BlockType::SNOWY_GRASS:
    case BlockType::SAND:
    case BlockType::SNOW: return 0.6f;
    case BlockType::CACTUS: return 0.6f;
    case BlockType::ICE:
    case BlockType::LAMP:
    case BlockType::CRUMBLING_BRICK: return 1.0f;
    case BlockType::PRESSURE_PLATE: return 1.5f;
    case BlockType::LOG: return 2.0f;
    case BlockType::PLANKS: return 1.5f;
    case BlockType::CHEST: return 1.5f;
    case BlockType::BED: return 0.8f;
    case BlockType::LADDER: return 0.5f;
    case BlockType::CAMPFIRE: return 0.6f;
    case BlockType::SIGN: return 0.5f;
    case BlockType::TNT:
    case BlockType::TNT_LIT: return 0.3f;
    case BlockType::DOOR:
    case BlockType::DOOR_OPEN: return 1.0f;
    case BlockType::GLASS: return 0.5f;
    case BlockType::SPIKES: return 2.0f;
    case BlockType::PUMP:
    case BlockType::PIPE:
    case BlockType::AIR_PIPE:
    case BlockType::DRILL:
    case BlockType::COMPRESSOR:
    case BlockType::AIR_TANK:
    case BlockType::VALVE:
    case BlockType::VALVE_OPEN:
    case BlockType::LOGIC_GATE:
    case BlockType::PISTON:
    case BlockType::PISTON_HEAD:
    case BlockType::PRESSURE_SENSOR:
    case BlockType::ITEM_PIPE:
    case BlockType::SMELTER:
    case BlockType::SMELTER_LIT:
    case BlockType::EXTRACTOR:
    case BlockType::FILTER_PIPE:
    case BlockType::CRAFTER:
    case BlockType::HARVESTER:
    case BlockType::CRUSHER:
    case BlockType::SLUICE: return 3.0f;
    case BlockType::WATER_SENSOR:
    case BlockType::MACHINE_SENSOR:
    case BlockType::DAYLIGHT_SENSOR:
    case BlockType::PLAYER_SENSOR: return 1.0f;
    case BlockType::SIGNAL_LAMP:
    case BlockType::SIGNAL_LAMP_ON:
    case BlockType::PRESSURE_GAUGE: return 0.5f;
    case BlockType::LEVER:
    case BlockType::LEVER_ON: return 0.4f;
    case BlockType::SHAFT:
    case BlockType::WATER_WHEEL: return 2.0f;
    case BlockType::STONE:
    case BlockType::SPRING:
    case BlockType::SINKHOLE:
    case BlockType::OIL_SHALE: return 4.0f;
    case BlockType::TAR: return 1.0f;
    case BlockType::MINING_POST: return 1.5f;
    case BlockType::STEEL_FRAME:
    case BlockType::MULTIBLOCK:
    case BlockType::MULTIBLOCK_PART: return 3.0f;
    case BlockType::MACHINE_CASING: return 4.0f;
    case BlockType::FIRE_BRICK: return 5.0f;
    case BlockType::FLUID_PIPE:
    case BlockType::FLUID_TANK:
    case BlockType::OIL_DERRICK:
    case BlockType::REFINERY:
    case BlockType::ENGINE:
    case BlockType::BOILER:
    case BlockType::TURBINE:
    case BlockType::AIR_DIFFUSER:
    case BlockType::FLUID_OUTLET: return 3.0f;
    case BlockType::DRAIN: return 3.0f;
    case BlockType::COAL_ORE: return 4.5f;
    case BlockType::IRON_ORE: return 6.0f;
    case BlockType::GOLD_ORE: return 7.0f;
    case BlockType::MAGMA: return 3.0f;
    case BlockType::STONE_BRICK:
    case BlockType::MOSSY_BRICK: return 5.0f;
    default: return 1.0f;
    }
}

// The tool that's faster on this type (Tool::None: nothing helps): the game's own.
inline Tool DefaultBestTool(BlockType type) {
    switch (type) {
    case BlockType::STONE:
    case BlockType::SPRING:
    case BlockType::DRAIN:
    case BlockType::SINKHOLE:
    case BlockType::OIL_SHALE:
    case BlockType::FLUID_PIPE:
    case BlockType::FLUID_TANK:
    case BlockType::OIL_DERRICK:
    case BlockType::REFINERY:
    case BlockType::ENGINE:
    case BlockType::BOILER:
    case BlockType::TURBINE:
    case BlockType::AIR_DIFFUSER:
    case BlockType::FLUID_OUTLET:
    case BlockType::STEEL_FRAME:
    case BlockType::MACHINE_CASING:
    case BlockType::FIRE_BRICK:
    case BlockType::MULTIBLOCK:
    case BlockType::MULTIBLOCK_PART:
    case BlockType::COAL_ORE:
    case BlockType::IRON_ORE:
    case BlockType::GOLD_ORE:
    case BlockType::MAGMA:
    case BlockType::STONE_BRICK:
    case BlockType::MOSSY_BRICK:
    case BlockType::CRUMBLING_BRICK:
    case BlockType::ICE:
    case BlockType::LAMP:
    case BlockType::SPIKES:
    case BlockType::PUMP:
    case BlockType::PIPE:
    case BlockType::AIR_PIPE:
    case BlockType::DRILL:
    case BlockType::COMPRESSOR:
    case BlockType::AIR_TANK:
    case BlockType::VALVE:
    case BlockType::VALVE_OPEN:
    case BlockType::LOGIC_GATE:
    case BlockType::PISTON:
    case BlockType::PISTON_HEAD:
    case BlockType::WATER_SENSOR:
    case BlockType::MACHINE_SENSOR:
    case BlockType::PRESSURE_SENSOR:
    case BlockType::DAYLIGHT_SENSOR:
    case BlockType::PLAYER_SENSOR:
    case BlockType::ITEM_PIPE:
    case BlockType::SMELTER:
    case BlockType::SMELTER_LIT:
    case BlockType::EXTRACTOR:
    case BlockType::FILTER_PIPE:
    case BlockType::CRAFTER:
    case BlockType::HARVESTER:
    case BlockType::CRUSHER:
    case BlockType::SLUICE:
    case BlockType::PRESSURE_PLATE: return Tool::Pickaxe;
    case BlockType::WATER_WHEEL:
    case BlockType::SHAFT:
    case BlockType::LOG:
    case BlockType::PLANKS:
    case BlockType::CHEST:
    case BlockType::BED:
    case BlockType::LADDER:
    case BlockType::DOOR:
    case BlockType::DOOR_OPEN:
    case BlockType::CACTUS: return Tool::Axe;
    case BlockType::DIRT:
    case BlockType::GRASS:
    case BlockType::DRY_GRASS:
    case BlockType::LUSH_GRASS:
    case BlockType::SNOWY_GRASS:
    case BlockType::SAND:
    case BlockType::SNOW: return Tool::Shovel;
    case BlockType::LEAVES:
    case BlockType::PINE_LEAVES: return Tool::Sword;
    default: return Tool::None;
    }
}

// What's used: Resources/Data/blocks.json's, if it says (world/BlockData.h).
inline float Hardness(BlockType type) {
    const auto set = BlockData::Hardness(type);
    return set ? *set : DefaultHardness(type);
}
inline Tool BestTool(BlockType type) {
    const auto set = BlockData::BestTool(type);
    return set ? *set : DefaultBestTool(type);
}

// Seconds it takes to break this block with this tool (in survival).
// `tier`: what the tools are made of (Crafting::Tier: 0 stone, 1 iron, 2 steel).
inline float Seconds(const Block& block, Tool tool, int tier = 0) {
    float seconds = Hardness(block.type);
    if (seconds <= 0.0f) return 0.0f;
    const Tool best = BestTool(block.type);
    if (best != Tool::None && tool == best) seconds /= tier >= 2 ? kSteelToolSpeed : tier == 1 ? kIronToolSpeed : kToolSpeed;
    if (!block.IsFullBlock() && block.CanChisel())
        seconds *= std::max(kMinVolumeShare, block.ResourceUnits() / static_cast<float>(Block::kUnitsPerBlock));
    return seconds;
}

inline Tool ToolFromId(const std::string& id) {
    for (int t = 1; t < static_cast<int>(Tool::Count); t++)
        if (id == ToolId(static_cast<Tool>(t))) return static_cast<Tool>(t);
    return Tool::None;
}

} // namespace Mining
