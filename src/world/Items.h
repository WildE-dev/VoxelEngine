#pragma once
// Item types: things you carry that aren't blocks and can't be placed in the world as
// themselves (ingots, food, arrows, the hammer). Blocks are BlockType (world/Block.h);
// tools and armour have their own enums. All of them are items in an inventory, by
// ItemId (world/Inventory.h).
//
// Some items do something to the world when used: seeds plant a crop (BlockType::CROP),
// a boat is launched on water, a bucket scoops water up. That's the item's use, not
// the item placed.
//
// Their details (name, icon) are data: Resources/Data/items, a file per item (ItemData
// below), like blocks'.
#include <cstdint>
#include <optional>
#include <string>

// (Saved by number, as an ItemId: new types go at the end, before COUNT.)
enum class ItemType : uint8_t {
    NONE,
    // Food (world/Hunger.h): venison from deer, fish from a rod, wheat baked to bread.
    RAW_VENISON,
    COOKED_VENISON,
    RAW_FISH,
    COOKED_FISH,
    BREAD,
    // Farming: seeds (from tall grass) planted on soil grow into a crop that ripens to
    // wheat.
    SEEDS,
    WHEAT,
    // Deer hide, for armour (world/Armour.h).
    HIDE,
    // Coal (from coal ore, or a log burnt) and coke (coal baked in a coke oven).
    COAL,
    COKE,
    // Metals, smelted or crafted (world/Smelting.h, world/Crafting.h): ingots of iron,
    // copper, gold (what dwarf traders pay and take) and steel (only a smelter makes it);
    // crushed iron (a crusher's, from ore: each smelts to two ingots).
    IRON_INGOT,
    COPPER_INGOT,
    GOLD_INGOT,
    STEEL_INGOT,
    CRUSHED_IRON,
    // Bitumen: what's left of crude oil once a refinery has made fuel of it. Cement:
    // burnt limestone, for concrete.
    BITUMEN,
    CEMENT,
    // Held and used: arrows (what a bow or crossbow shoots); a crossbow (right-click
    // shoots a bolt harder, faster and flatter than a bow, but more slowly:
    // Server::HandleShoot); a boat (right-click water to get in); the engineer's hammer
    // (forms multiblocks, world/Multiblocks.h); a bucket, which scoops up a block of
    // water (a water bucket) and pours it out again, putting out fires where it lands.
    ARROW,
    CROSSBOW,
    BOAT,
    HAMMER,
    BUCKET,
    WATER_BUCKET,
    // A forge golem's core: what it leaves when it falls. A steam turbine needs one.
    FORGE_CORE,
    COUNT
};

// A short id for an item type ("iron_ingot"), for commands and data; and back
// (ItemType::COUNT if there's no such id).
const char* ItemTypeId(ItemType type);
ItemType ItemTypeFromId(const std::string& id);
// The game's own name ("Iron ingot") and icon (a texture in Resources/Textures/Blocks).
const char* DefaultItemName(ItemType type);
const char* DefaultItemTexture(ItemType type);

namespace ItemData {

// Reads Resources/Data/items (a file per item, by id) or a single file of them:
// { "iron_ingot": { "name": "Iron ingot", "texture": "iron_ingot" }, ... }. What an
// item doesn't say keeps the game's own. False (with why) if it can't be read.
bool Load(const std::string& path, std::string& error);
constexpr const char* kPath = "Resources/Data/items";

std::optional<std::string> Name(ItemType t);
std::optional<std::string> Texture(ItemType t);
// The file's, else the game's own (good until the next Load).
const char* NameOf(ItemType t);
const char* TextureOf(ItemType t);

} // namespace ItemData
