#pragma once
// Steam: a gas in the open air, kept by the server cell by cell (not in the blocks: a
// cloud changes every step). Each cell holds some units of it. Every step (kInterval)
// a cell shares what it has with the open cells around it that have less: most goes
// up (it rises; thick steam hard, thin steam hardly at all), some to each side, a
// little down, so a burst billows up, spreads out under anything over it and hangs,
// thinning as it grows. It fades too (kFadePerThousand
// of what's in a cell each step, at least a unit), so a cloud swells, thins and is
// gone in a quarter of a minute or so. Open cells are air or plants: never solid
// blocks, water or lava. A block put where steam is pushes it up out of the way.
//
// Lava makes it, boiling away water that touches it (world/Lava.h). Evaporation
// (server/Water.h) doesn't: puddles just dry up.
//
// The server sends the steam near each player a few times a second (net::SteamNear),
// and the client draws it as a translucent cloud filling those cells (render/SteamVolume.h).
// Thick steam (kScaldUnits) scalds players in it; thin steam is only to see.
#include <cstdint>
#include <unordered_map>

#include <glm/glm.hpp>

#include "world/Coords.h"

class BlockAccess;

class SteamCloud
{
public:
    static constexpr double kInterval = 0.25;   // seconds between steps (its edge moves a cell a step: it rises 4 blocks a second at most)
    static constexpr int kFadePerThousand = 22; // of each cell, every step (and a unit off thin ones)
    static constexpr int kMin = 2;              // less than this and it's gone
    static constexpr int kMaxCell = 8000;       // most a cell holds
    static constexpr size_t kMaxCells = 16000;  // cells of steam in the world, at most (a step costs about a millisecond per thousand)
    // Thick steam scalds: a player with this much in a cell their body is in is hurt
    // kScaldDamage every kScaldInterval seconds (armour doesn't help).
    static constexpr int kScaldUnits = 200;
    static constexpr int kScaldDamage = 1;
    static constexpr double kScaldInterval = 0.75;

    using Cells = std::unordered_map<glm::ivec3, uint16_t, Coords::IVec3Hash>;

    // Puts steam in a cell (it spreads out from there).
    void Add(const glm::ivec3& cell, int units);
    // One step: sharing out, rising, fading.
    void Update(const BlockAccess& world);

    const Cells& All() const { return cells; }
    int At(const glm::ivec3& cell) const {
        auto it = cells.find(cell);
        return it == cells.end() ? 0 : it->second;
    }
    long long Total() const;
    void Clear() { cells.clear(); }

    // Whether steam can be in this cell: open (air or a plant), dry, not lava.
    static bool Open(const BlockAccess& world, const glm::ivec3& cell);

private:
    Cells cells;
};
