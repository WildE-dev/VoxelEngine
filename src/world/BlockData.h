#pragma once
// Blocks' editable details, from Resources/Data/blocks.json: for each block (by its
// id, "stone"), any of
//   "name": "Stone"                                  what the game calls it
//   "textures": { "all": "stone" }                   or "top", "bottom" and "side",
//                                                    or each of "top", "bottom",
//                                                    "front", "back", "left", "right"
//                                                    (names of PNGs in
//                                                    Resources/Textures/Blocks)
//   "hardness": 4.0                                  seconds to break by hand
//   "tool": "pickaxe"                                the tool that's faster on it
//                                                    (pickaxe, axe, shovel, sword,
//                                                    or none)
//   "sound": "hard"                                  what it sounds like: soft, hard,
//                                                    sand, wood or glass
//   "light": 0                                       light it gives off, 0 to 15
//   "map": "#808080"                                 its colour on the minimap
// What a block doesn't say keeps the game's own value (so a block left out of the
// file, or a missing file, is as it always was). Loaded at start (the game and the
// server) and again with the R key. How blocks behave (solid, plant, liquid, machine)
// stays in the code.
#include <cstdint>
#include <optional>
#include <string>

#include "world/Block.h"

enum class Tool : uint8_t;

namespace BlockData {

// Reads the file; false (with why) if it couldn't. Blocks it names are set from it,
// the rest go back to the game's own values.
bool Load(const std::string& path, std::string& error);
constexpr const char* kPath = "Resources/Data/blocks"; // (a file per block; or blocks.json, all in one)

// What the file set (nothing: use the game's own).
std::optional<std::string> Name(BlockType t);
std::optional<std::string> Texture(BlockType t, int face); // face: 0 top, 1 bottom, 2 front, 3 back, 4 left, 5 right
std::optional<float> Hardness(BlockType t);
std::optional<Tool> BestTool(BlockType t);
std::optional<std::string> Sound(BlockType t);
std::optional<uint32_t> MapColour(BlockType t); // 0xAABBGGRR
// The name or sound the file set, else `fallback` (good until the next Load).
const char* NameOr(BlockType t, const char* fallback);
const char* SoundOr(BlockType t, const char* fallback);

// Light overrides, read where light is worked out (-1: none).
extern int8_t gLight[static_cast<size_t>(BlockType::COUNT)];

} // namespace BlockData
