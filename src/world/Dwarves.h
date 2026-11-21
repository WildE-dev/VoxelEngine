#pragma once
// Dwarf camps and dwarf mines: where the dwarves live (creatures in world/Creatures.h).
//
// The world is split into kRegionSize x kRegionSize regions; a region may hold a camp
// or a mine (not both), laid out from a seed hashed from its coordinates, on dry
// ground that's flat enough. Like dungeons (world/Dungeons.h), everything is a
// function of world position, so a chunk stamps just the part inside it (Stamp).
//
//  - A camp: a ring of trodden earth inside a palisade of log posts with a gap for a
//    gate, huts of planks round a campfire, and a chest of loot.
//  - A market: a camp of friendly dwarves (the same palisade, huts and fire, lamps
//    either side of the gate and no loot): a trader (world/Trades.h) and guards, who
//    fight hostile dwarves that come near, and anyone who hits one of them.
//  - A stronghold (rare): a great hall of stone bricks kHallDepth below the ground,
//    23 blocks square and 8 high, with pillars, lamps and walls rich in ore, a throne
//    at the far end and a vault behind it, reached by a stairway down from a
//    gatehouse on the surface. Its Dwarf King (a boss) and his guards live there; the
//    king doesn't come back once he's been defeated.
//  - A mine: a timber headframe over a shaft three blocks across, with a ladder down
//    to kMineDepth below the ground, and tunnels three wide and three high running out
//    from its foot, framed every few blocks by log posts in the walls and a beam
//    overhead, lamps here and there, ore showing in the walls, and a chest at the end
//    of each tunnel.
// The server spawns hostile dwarves at a site's spawn points while players are near
// (SitesNear), and rolls its chests' loot (LootFor).
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "world/Block.h"
#include "world/Chunk.h"

class TerrainGenerator;

namespace Dwarves {

constexpr int kRegionSize = 160;
constexpr int kCampRadius = 7;
constexpr int kMineDepth = 20;

enum class SiteKind : uint8_t { Camp, Mine, Market, Stronghold };
constexpr int kHallDepth = 28;  // a stronghold's hall floor, below the ground over it
constexpr int kHallHalf = 10;   // its floor runs this far either way from the centre

struct Site {
    SiteKind kind = SiteKind::Camp;
    glm::ivec3 centre{ 0 };            // a camp's middle on its floor; a mine's shaft top
    std::vector<glm::ivec3> spawns;    // where dwarves stand (feet)
    std::vector<glm::ivec3> chests;
    int warriors = 0, crossbows = 0, chiefs = 0; // how many live there
    int traders = 0, guards = 0;                 // (a market's)
    int kings = 0;                               // (a stronghold's)
};

// The site in a region, if any (exists = false: none).
struct RegionSite {
    bool exists = false;
    Site site;
    uint32_t seed = 0;
    // A mine's tunnels: from the shaft's side outwards (unit direction), and how long.
    std::vector<std::pair<glm::ivec3, int>> tunnels;
    int bottom = 0; // a mine's tunnel floor
    glm::ivec3 gate{ 1, 0, 0 }; // a camp's gate direction (a stronghold's stairway's)
    int stairs = 0;             // a stronghold's stairway: steps up from the hall to the open
};
RegionSite SiteIn(const TerrainGenerator& terrain, int regionX, int regionZ);

// The sites within `range` blocks of a position.
std::vector<Site> SitesNear(const TerrainGenerator& terrain, const glm::vec3& pos, float range);

// Stamps the parts of the sites inside a chunk.
using Blocks = std::array<Block, Chunk::VOLUME>;
void Stamp(const TerrainGenerator& terrain, const glm::ivec3& chunkCoords, Blocks& out);

// What's in a camp's or mine's chest at `pos` (empty if there's none there).
std::vector<std::pair<BlockType, uint32_t>> LootFor(const TerrainGenerator& terrain, const glm::ivec3& pos);

} // namespace Dwarves
