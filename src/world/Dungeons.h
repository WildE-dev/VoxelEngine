#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "world/Chunk.h"

class TerrainGenerator;

// Underground dungeons: rooms of many kinds joined by corridors, deep below the
// surface, some with a spiral staircase up to a ring of bricks on the surface.
//
// The world is split into kRegionSize x kRegionSize regions, and each region may hold
// one dungeon, laid out from a seed hashed from the region's coordinates (LayoutFor).
// Everything a dungeon places is a function of world position, so a chunk stamps just
// the part inside it (Stamp) and neighbouring chunks agree at their borders. Stamping
// runs in three passes over the chunk: brick shells (walls, floors, ceilings), then
// interiors (air: where a corridor meets a room it opens a doorway), then furnishings.
//
// Rules the rest of generation relies on: everything is well below the surface apart
// from the entrance shaft, which only removes blocks below the surface (like a cave)
// and puts a ring of bricks on top of the ground, never over open air. Water sits in
// basins sealed by bricks, so it stays put until disturbed.
//
// Traps: some long corridors have a spike pit across them, either hidden under
// crumbling bricks (three long) or open (two long, jumpable); vaults' pedestals are
// ringed by crumbling floor over spikes; flood chambers flood when their plate is
// stepped on. (The server makes the blocks work: see Server::UpdateTraps.)
namespace Dungeons {

constexpr int kRegionSize = 128;

enum class RoomType : uint8_t {
    Entrance,  // spiral staircase up a shaft to the surface
    Hall,      // tall room with rows of pillars topped with lamps
    Archive,   // shelves (logs) along the walls, a lamp over a table
    Pool,      // a sunken pool of water
    Garden,    // overgrown: grass floor, bushes, tall grass, mossy walls
    Crypt,     // low room with rows of stone tombs
    Vault,     // lamps set in the floor around a lit pedestal
    Collapsed, // heaps of rubble
    Cistern,   // a deep pit with water at the bottom, pillars and a ledge around
    PumpRoom,  // a pump lifting water from a sunken basin up into a raised tub
    Shrine,    // a lamp on a plinth with pews (logs) facing it
    FloodChamber, // a pressure plate opens sluice gates in the ceiling: the reservoir above floods in
    Count
};
const char* RoomName(RoomType type);

struct Box {
    glm::ivec3 min{0}, max{0}; // inclusive
    bool Contains(const glm::ivec3& p) const { return glm::all(glm::greaterThanEqual(p, min)) && glm::all(glm::lessThanEqual(p, max)); }
};

struct Room {
    RoomType type = RoomType::Hall;
    Box inner;        // the open space; the floor is the row below inner.min.y
    uint32_t seed = 0;
    // Pump rooms: the row (z) the basin, pipe and tub line up on, and whether the basin
    // is at the high-x end. Chosen so the raised tub stays out of every corridor.
    int lane = 0;
    bool flip = false;
};

struct Layout {
    bool exists = false;
    int floorY = 0;              // the rooms' and corridors' lowest open row
    std::vector<Room> rooms;     // rooms[0] is the entrance
    std::vector<Box> corridors;  // open space of each corridor segment
    struct Pit {
        Box open;        // the hole under the corridor floor (its top row is the floor's row)
        bool covered;    // under crumbling bricks, or open
    };
    std::vector<Pit> pits;       // spike pits across corridors
    Box bounds;                  // everything, shells and shaft included
    bool hasShaft = false;       // whether the entrance reaches the surface
    glm::ivec2 shaft{0};         // shaft centre column
    int shaftTop = 0;            // first open row above the ground at the shaft
};

// The dungeon of a region (exists is false for regions without one). Deterministic.
Layout LayoutFor(const TerrainGenerator& terrain, int regionX, int regionZ);

// Stamps the dungeon parts inside a chunk over generated terrain.
void Stamp(const TerrainGenerator& terrain, const glm::ivec3& chunkCoords, std::array<Block, Chunk::VOLUME>& blocks);

} // namespace Dungeons
