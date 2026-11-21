#pragma once
// Multiblock structures: big machines built from a pattern of blocks, then formed by
// striking any of its blocks with an engineer's hammer (BlockType::HAMMER, held).
//
// Each pattern is 3 x 3 across and some blocks high, round a machine block (its core),
// in any of four turns about the vertical. Formed, the core becomes a MULTIBLOCK block
// (its `axis`: the kind times 4, plus the turn) and the other blocks become
// MULTIBLOCK_PART blocks (solid, drawn as nothing: the core draws the whole structure's
// model, from Resources/Data/structures.json), except its ports, which stay as they
// are: an air pipe for its air, fluid pipes for what flows in and out, an item pipe for
// the chests it works from. Breaking any of it takes it apart: every block goes back
// to what it was (the one broken gives itself up as usual).
//
//  - Oil derrick: steel frames round an oil derrick block, a tower above. It pumps
//    crude oil out of the shale under its 3 x 3 footprint, down to kWellDepth, into the
//    fluid pipes at its crude port, three times as fast as a derrick block alone did.
//  - Refinery: machine casings round a refinery block, with fluid tanks and a column.
//    Crude oil in at one fluid port, fuel out of the other, three times as fast.
//  - Blast furnace: fire bricks round a smelter. It smelts from the chests joined to its
//    item port, four times as fast as a smelter, for less air a block.
//  - Diesel compressor: machine casings and fluid tanks round an engine. It burns fuel
//    from the fluid pipes at its fuel port (kDieselFuel a second) and puts
//    kDieselAir a second into the air network at its air port: three times an
//    engine's air for twice its fuel.
//  - Coke oven: stone bricks round a smelter, with a chimney. It bakes two coal into a
//    coke every kCokeInterval, from the chests joined to its item port, for air. Coke
//    makes steel for half the coal (world/Smelting.h).
// The server runs them (Server::UpdatePower); patterns are below, models in the data.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "world/Block.h"

namespace Multiblocks {

enum class Kind : uint8_t { OilDerrick, Refinery, BlastFurnace, DieselCompressor, CokeOven, Count };
constexpr float kDieselFuel = 32.0f;     // fuel units a second a diesel compressor burns
constexpr float kDieselAir = 72.0f;      // air a second it makes
constexpr double kCokeInterval = 3.0;    // seconds per coke

enum class Role : uint8_t { Ignore, Part, Core, AirPort, FluidIn, FluidOut, ItemPort };

struct Cell {
    glm::ivec3 at;  // in the pattern: x and z 0..2 across, y up from 0
    BlockType type; // what must be there
    Role role;
};

struct Pattern {
    Kind kind;
    const char* id;   // "oil_derrick" (its model's name in structures.json)
    const char* name; // "Oil derrick"
    std::vector<Cell> cells;
};

const Pattern& PatternOf(Kind k);

// A cell of a pattern placed in the world: `origin` is the pattern's (0, 0, 0) corner,
// `turn` 0..3 quarter turns (the pattern's x and z turned about its middle column).
inline glm::ivec3 Place(const glm::ivec3& origin, int turn, const glm::ivec3& at) {
    int x = at.x, z = at.z;
    for (int t = 0; t < (turn & 3); t++) {
        const int nx = 2 - z, nz = x;
        x = nx, z = nz;
    }
    return origin + glm::ivec3(x, at.y, z);
}

// The core's encoding in its axis byte, and back.
inline uint8_t Encode(Kind k, int turn) { return static_cast<uint8_t>(static_cast<int>(k) * 4 + (turn & 3)); }
inline Kind KindOf(const Block& core) { return static_cast<Kind>(std::min<int>(core.axis / 4, static_cast<int>(Kind::Count) - 1)); }
inline int TurnOf(const Block& core) { return core.axis & 3; }
// Where a formed structure's pattern starts, from its core.
glm::ivec3 OriginOf(const glm::ivec3& corePos, const Block& core);
// The world position of a pattern's first cell with this role (a port).
bool PortOf(const glm::ivec3& corePos, const Block& core, Role role, glm::ivec3& out);

} // namespace Multiblocks
