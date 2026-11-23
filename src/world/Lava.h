#pragma once
// Lava: a glowing liquid that creeps slowly, simpler than water (server/Water.h). A
// source stays put; lava pours down over edges as a falling column, and on the level it
// runs up to kReach blocks out from a source (or from where a fall lands), thinner each
// block. Flowing lava with nothing feeding it any more drains away. Its `axis` holds its
// level (0 a source, 1..kReach how far it has run) and whether it's falling (kFalling).
//
// Water touching lava boils away at once into a big cloud of steam (kSteamPerUnit for
// every unit of water; server/Steam.h), whether the water flows up to the lava or the
// lava to the water. The lava stays.
//
// The server runs it (Server::UpdateLava, every kInterval): the lava cells woken by a
// change beside them take a Step, and what it says changes is set in the world, which
// wakes the cells around those in turn.
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "world/BlockAccess.h"
#include "world/Coords.h"

namespace Lava {

constexpr double kInterval = 0.5;  // seconds between steps (lava is slow)
constexpr int kReach = 3;          // blocks it runs on the level
constexpr uint8_t kFalling = 4;    // in `axis`
constexpr int kSteamPerUnit = 40;  // steam made by each unit of water it boils away

inline int Level(const Block& b) { return b.axis & 3; }
inline bool Falling(const Block& b) { return (b.axis & kFalling) != 0; }
inline bool IsSource(const Block& b) { return b.IsLava() && Level(b) == 0 && !Falling(b); }
inline Block Make(int level, bool falling) {
    Block b(BlockType::LAVA);
    b.axis = static_cast<uint8_t>((level & 3) | (falling ? kFalling : 0));
    return b;
}
// How high its surface stands in its cell, in eighths: a source 7, a fall right up, and
// thinner the further it has run.
inline int HeightEighths(const Block& b) { return Falling(b) ? 8 : Level(b) == 0 ? 7 : 8 - 2 * Level(b); }
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

// One step for these lava cells: what each turns into (drained away, thinner, falling),
// where it runs, and the water it boils away (and the steam that makes). Where two cells
// run into the same one, the fuller lava wins.
inline void Step(const BlockAccess& world, const std::vector<glm::ivec3>& cells, std::vector<Change>& changes, std::vector<Boil>& boils) {
    std::unordered_map<glm::ivec3, Block, Coords::IVec3Hash> out;
    auto put = [&](const glm::ivec3& p, const Block& b) {
        auto it = out.find(p);
        if (it == out.end()) out.emplace(p, b);
        else if (b.IsLava() && it->second.IsLava() && HeightEighths(b) > HeightEighths(it->second)) it->second = b;
    };
    static const glm::ivec3 kSides[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const glm::ivec3& p : cells) {
        Block b;
        if (!world.GetBlock(p.x, p.y, p.z, b) || !b.IsLava()) continue;
        // Water touching it boils away (and doesn't flow on into it).
        for (int f = 0; f < kFaceCount; f++) {
            const glm::ivec3 n = p + glm::ivec3(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]);
            Block w;
            if (!world.GetBlock(n.x, n.y, n.z, w) || !w.HasWater()) continue;
            if (auto done = out.find(n); done != out.end() && !done->second.IsLava()) continue; // (already boiled, by other lava)
            boils.push_back({ n, w.WaterVolume() * kSteamPerUnit });
            Block dry = w.IsLiquid() ? Block() : w;
            dry.water = dry.shown = 0;
            out[n] = dry; // (boiling wins over lava running in)
        }
        // Flowing lava keeps going only while something feeds it: lava above (it falls),
        // or fuller lava beside it.
        if (!IsSource(b)) {
            Block above;
            const bool fed = world.GetBlock(p.x, p.y + 1, p.z, above) && above.IsLava();
            int best = kReach + 1;
            for (const auto& d : kSides) {
                Block n;
                if (!world.GetBlock(p.x + d.x, p.y, p.z + d.z, n) || !n.IsLava()) continue;
                const int theirs = Falling(n) ? 0 : Level(n);
                best = std::min(best, theirs + 1);
            }
            const Block should = fed ? Make(0, true) : best <= kReach ? Make(best, false) : Block();
            if (!(should == b)) {
                put(p, should);
                continue; // (it runs on from what it is now, next step)
            }
        }
        // Down if it can; on the level (not while it can fall) out to its reach.
        Block below;
        if (!world.GetBlock(p.x, p.y - 1, p.z, below)) continue;
        if (CanFlowInto(below)) {
            put(p - glm::ivec3(0, 1, 0), Make(0, true));
            continue;
        }
        if (below.IsLava()) continue; // (falling into lava: it's fed from here)
        const int level = Falling(b) ? 0 : Level(b);
        if (level >= kReach) continue;
        for (const auto& d : kSides) {
            Block n;
            if (world.GetBlock(p.x + d.x, p.y, p.z + d.z, n) && CanFlowInto(n)) put(p + d, Make(level + 1, false));
        }
    }
    for (const auto& [pos, block] : out) changes.push_back({ pos, block });
}

} // namespace Lava
