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
//   "collision": { ... }                             what it's bumped into, stood on,
//                                                    aimed at and outlined as (below)
// What a block doesn't say keeps the game's own value (so a block left out of the
// file, or a missing file, is as it always was). Loaded at start (the game and the
// server) and again with the R key. How blocks behave (plant, liquid, machine) stays in
// the code.
//
// "collision" (all of it optional):
//   "boxes": [ { "from": [x, y, z], "to": [x, y, z] }, ... ]
//       boxes in the cell (0..1 each way) it fills instead of the whole cell (a pipe,
//       a shaft, a door's panel). A string in the list ("pipe") adds that block's
//       boxes; "boxes": "pipe" alone is the same boxes as that block. At most
//       Block::kMaxModelBoxes of them count at once. A box may only count when:
//         "joined": ["+x", ...]       all of these faces join a neighbour (a pipe's arms)
//         "joinedAny": ["+y", ...]    any of them do
//         "joinedNone": ["+x", ...]   none of them do
//         "otherwise": true           the box before it (or its run of these) didn't
//   "turns": true     the boxes are drawn for a block pointing +z (for one along a grain
//                     axis, along z) and turn the way it points
//   "mirrors": true   mirrored left to right (x) for a door hung on its right
//   "solid": false    walked through (only aimed at); plants aren't solid, the rest are
//   "floor": false    never stood on (a door's top)
// Without boxes a solid block fills its cell, or its shape if it's chiselled or a slope.
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

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

// A block's "collision" (above), its references to other blocks' boxes followed.
struct ShapeBox {
    glm::vec3 lo{ 0.0f }, hi{ 1.0f };
    uint8_t joinedAll = 0, joinedAny = 0, joinedNone = 0; // faces (a bit each, Face order)
    bool otherwise = false;
};
struct Shape {
    bool hasBoxes = false;
    std::vector<ShapeBox> boxes;
    bool turns = false, mirrors = false;
    std::optional<bool> solid, floor;
};
const Shape& ShapeOf(BlockType t);

// Light overrides, read where light is worked out (-1: none): the current table.
extern std::atomic<const int8_t*> gLight;

} // namespace BlockData
