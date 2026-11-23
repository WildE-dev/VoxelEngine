#pragma once
// Lava: a glowing liquid that flows like water (server/Water.h), only slower and
// thicker. A lava cell holds 1..kFull units (`axis`, 0 meaning full: a full block, as
// lava placed or generated is). It's only ever moved, like water: each step it falls,
// as much as fits, into open space or thinner lava below it; with nowhere to fall it
// levels out with the cells beside it, sharing with lava lower than it and with open
// cells. It doesn't thin out past kMinDepth, so a block of lava ends as a small pool,
// not a film over everything. Steps come every kInterval: a tenth of water's pace.
//
// Water touching lava boils away at once into a big cloud of steam (kSteamPerUnit for
// every unit of water; server/Steam.h), whether the water flows up to the lava or the
// lava to the water. The lava stays.
//
// The server runs it (Server::UpdateLava): the lava cells woken by a change beside them
// take a Step, and what it says changes is set in the world, which wakes the cells
// around those in turn. Lava at rest isn't woken, so it costs nothing.
#include <algorithm>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "world/BlockAccess.h"
#include "world/Coords.h"

namespace Lava {

constexpr double kInterval = 0.5; // seconds between steps (a tenth of water's pace)
constexpr int kFull = 128;        // units in a full block of it (as water)
constexpr int kMinDepth = 32;     // it doesn't spread thinner than a quarter of a block
constexpr int kSteamPerUnit = 40; // steam made by each unit of water it boils away

// How much a lava cell holds (0 for anything else).
inline int Volume(const Block& b) { return !b.IsLava() ? 0 : b.axis == 0 || b.axis > kFull ? kFull : b.axis; }
inline Block WithVolume(int units) {
    if (units <= 0) return Block();
    Block b(BlockType::LAVA);
    b.axis = static_cast<uint8_t>(units >= kFull ? 0 : units);
    return b;
}
// How high its surface stands in its cell, in eighths (at least one).
inline int HeightEighths(const Block& b) { return std::clamp((Volume(b) * 8 + kFull - 1) / kFull, 1, 8); }
// Where it may run: open air, or a plant (burnt up), with no water in it.
inline bool CanFlowInto(const Block& b) { return b.IsReplaceableByWater() && !b.HasWater(); }

struct Change {
    glm::ivec3 pos;
    Block block;
};
struct Boil {
    glm::ivec3 pos;
    int steam; // units of steam made there
};

// One step for these lava cells: the water they boil away (and the steam that makes),
// and where their lava goes (falling, then levelling out). Lava is only moved, never
// made or lost.
inline void Step(const BlockAccess& world, const std::vector<glm::ivec3>& cells, std::vector<Change>& changes, std::vector<Boil>& boils) {
    // What each touched cell holds now, as this step goes (the world's, till changed).
    std::unordered_map<glm::ivec3, Block, Coords::IVec3Hash> now;
    auto at = [&](const glm::ivec3& p, Block& b) {
        auto it = now.find(p);
        if (it != now.end()) {
            b = it->second;
            return true;
        }
        return world.GetBlock(p.x, p.y, p.z, b);
    };
    static const glm::ivec3 kSides[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const glm::ivec3& p : cells) {
        Block b;
        if (!at(p, b) || !b.IsLava()) continue;
        // Water touching it boils away.
        for (int f = 0; f < kFaceCount; f++) {
            const glm::ivec3 n = p + glm::ivec3(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]);
            Block w;
            if (!at(n, w) || !w.HasWater()) continue;
            boils.push_back({ n, w.WaterVolume() * kSteamPerUnit });
            Block dry = w.IsLiquid() ? Block() : w;
            dry.water = dry.shown = 0;
            now[n] = dry;
        }
        int v = Volume(b);
        // Fall: as much as fits into open space or thinner lava below.
        Block below;
        const glm::ivec3 down = p - glm::ivec3(0, 1, 0);
        if (at(down, below) && (CanFlowInto(below) || (below.IsLava() && Volume(below) < kFull))) {
            const int move = std::min(v, kFull - Volume(below));
            now[down] = WithVolume(Volume(below) + move);
            v -= move;
            now[p] = WithVolume(v);
            continue; // (it levels out once it's landed)
        }
        // Level out: a share of the difference to each lower neighbour (lava thinner than
        // it, or open space, while it's at least twice as deep as it may thin to).
        int give[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 4; i++) {
            Block n;
            if (!at(p + kSides[i], n)) continue;
            const int theirs = Volume(n);
            if (n.IsLava() ? theirs < v - 1 : (CanFlowInto(n) && v >= 2 * kMinDepth)) give[i] = (v - theirs) / 5;
        }
        for (int i = 0; i < 4; i++) {
            if (give[i] <= 0) continue;
            Block n;
            at(p + kSides[i], n);
            now[p + kSides[i]] = WithVolume(Volume(n) + give[i]);
            v -= give[i];
        }
        now[p] = WithVolume(v);
    }
    for (const auto& [pos, block] : now) {
        Block was;
        if (!world.GetBlock(pos.x, pos.y, pos.z, was) || !(was == block)) changes.push_back({ pos, block });
    }
}

} // namespace Lava
