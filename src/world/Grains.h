#pragma once
// Sand and gravel: loose ground that gravity works on. They only ever have their slope
// on top (a shape turned on its side or upside down, or cut from underneath, settles
// back upright, keeping how much there is), and they run like a very thick liquid:
//
//  - a cell with room under it falls: into open space as it is, into a part-filled
//    cell of the same below as much as fits, and through water (which rises into the
//    space it left);
//  - a cell standing higher than the cell beside it by more than its angle of repose
//    (Repose: steeper for gravel than for sand) shares with it, half the difference
//    beyond that each step, so a heap spreads out until no side of it is steeper than
//    that: a pile, not a puddle;
//  - its shape follows the ground round it: each corner at the average height of the
//    cells that meet there (as the world's hills, and a rake, shape ground), so a pile
//    is drawn as smooth slopes rather than steps.
//
// How much a cell holds is its slope's mean height, in sixths of a sixteenth (the
// weighting Block::ResourceUnits gives a slope's corners: 2 for corners 0 and 1, 1 for
// 2 and 3): Amount, 0..kFull. Every step only moves that between cells, and each
// shape is made to hold exactly what its cell has, so none is ever made or lost.
// Waterlogged cells only fall; they don't spread or reshape (their water's room would
// change under it).
//
// The server runs it (Server::UpdateGrains) like lava: the cells woken by a change
// near them (ServerWorld::SetBlock) take a Step, and what changes wakes the cells round
// it in turn. A pile at rest isn't woken, so it costs nothing.
#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "world/BlockAccess.h"
#include "world/Coords.h"

namespace Grains {

constexpr double kInterval = 0.1;          // seconds between steps
constexpr int kSixths = 6;                 // a slope's mean height is kept in sixths of a sixteenth
constexpr int kFull = EdgeData::kSteps * kSixths; // a whole block: 96
constexpr int kMin = kSixths;              // the least a cell holds: every corner a pixel tall
constexpr size_t kMaxCellsPerStep = 4096;  // (the rest wait for the next step)

inline bool IsGrain(BlockType t) { return t == BlockType::SAND || t == BlockType::GRAVEL; }
inline bool IsGrain(const Block& b) { return IsGrain(b.type); }

// How much steeper than level it stands at rest: the most a cell may stand above the
// one beside it, in Amount's units (sand about 10 sixteenths a block, gravel 12).
inline int Repose(BlockType t) { return (t == BlockType::GRAVEL ? 12 : 10) * kSixths; }

// The weighted sum of a shape's corner heights (6 times its mean height, in sixteenths).
inline int Weighted(const std::array<int, 4>& h) { return h[2] + h[3] + 2 * (h[0] + h[1]); }

// How much this cell of sand or gravel holds (kFull for a whole block).
inline int Amount(const Block& b) {
    if (!IsGrain(b)) return 0;
    if (b.IsFullBlock()) return kFull;
    // Upright, from the floor: its corners' weighted heights. Otherwise (on its side,
    // upside down, cut from below) what it's worth, as an upright slope.
    bool upright = b.ShapeDir() == 0;
    std::array<int, 4> h{};
    for (int e = 0; e < 4 && upright; e++) {
        upright = !b.edgeData.IsEmpty(e) && b.edgeData.GetBottomY(e) == 0;
        h[e] = b.edgeData.GetTopY(e);
    }
    if (upright) return Weighted(h);
    return std::clamp((b.ResourceUnits() * kFull + Block::kUnitsPerBlock / 2) / Block::kUnitsPerBlock, 0, kFull);
}

// Whether the cell is in the shape Shape would give it: upright, from the floor.
inline bool Upright(const Block& b) {
    if (b.IsFullBlock()) return true;
    if (b.ShapeDir() != 0) return false;
    for (int e = 0; e < 4; e++)
        if (b.edgeData.IsEmpty(e) || b.edgeData.GetBottomY(e) != 0) return false;
    return true;
}

// A cell of `type` holding `amount`, its corners as near `want` (sixteenths up from the
// floor; EdgeData's corner order) as holding exactly that allows. Air if nothing.
inline Block Shape(BlockType type, int amount, std::array<int, 4> want) {
    if (amount <= 0) return Block();
    if (amount >= kFull) return Block(type);
    // Brought to hold exactly `amount`, keeping its slope: all of it raised or lowered
    // together (the weights sum to 6), then a sixteenth at a time on whichever corner has
    // been moved least from where it was wanted (the weight-1 corners for a last odd
    // one), so corners stopped at the top or bottom of the cell leave the rest to go on.
    const std::array<int, 4> aim = want;
    const int shift = (amount - Weighted(want)) / kSixths;
    for (int& h : want) h = std::clamp(h + shift, 1, EdgeData::kSteps);
    int delta = amount - Weighted(want);
    for (int guard = 0; delta != 0 && guard < 400; guard++) {
        const int sign = delta > 0 ? 1 : -1;
        int best = -1;
        for (int e = 0; e < 4; e++) {
            const int weight = e <= 1 ? 2 : 1;
            if (std::abs(delta) < weight) continue;
            const int next = want[e] + sign;
            if (next < 1 || next > EdgeData::kSteps) continue;
            if (best < 0 || sign * (want[e] - aim[e]) < sign * (want[best] - aim[best])) best = e;
        }
        if (best < 0 && std::abs(delta) == 1) {
            // One sixth left, and neither weight-1 corner can take it: a weight-2 corner
            // moves its way and a weight-1 corner back (2 - 1).
            int two = -1, one = -1;
            for (int e = 0; e <= 1; e++)
                if (want[e] + sign >= 1 && want[e] + sign <= EdgeData::kSteps) two = e;
            for (int e = 2; e <= 3; e++)
                if (want[e] - sign >= 1 && want[e] - sign <= EdgeData::kSteps) one = e;
            if (two >= 0 && one >= 0) {
                want[two] += sign;
                want[one] -= sign;
                delta = 0;
            }
            break;
        }
        if (best < 0) break; // (can't: every corner at its limit)
        want[best] += sign;
        delta -= sign * (best <= 1 ? 2 : 1);
    }
    EdgeData edges;
    for (int e = 0; e < 4; e++) edges.Set(e, want[e], 0);
    return Block(type, edges);
}
inline Block Flat(BlockType type, int amount) {
    const int h = (amount + kSixths / 2) / kSixths;
    return Shape(type, amount, { h, h, h, h });
}

struct Change {
    glm::ivec3 pos;
    Block block;
};

// One step for these cells (any of them: those not sand or gravel are passed over).
inline void Step(const BlockAccess& world, const std::vector<glm::ivec3>& cells, std::vector<Change>& changes, uint32_t stepNumber = 0) {
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
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> moved; // cells whose amount changed (not reshaped this step)
    auto set = [&](const glm::ivec3& p, const Block& b) { now[p] = b; };
    auto open = [](const Block& b) { return (b.IsAir() || b.IsReplaceablePlant()) && !b.HasWater(); };

    // Bottom up, so what's under falls before what's over it.
    std::vector<glm::ivec3> order(cells);
    std::sort(order.begin(), order.end(), [](const glm::ivec3& a, const glm::ivec3& b) { return a.y != b.y ? a.y < b.y : a.x != b.x ? a.x < b.x : a.z < b.z; });
    static const glm::ivec3 kSides[4] = { { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, 0, -1 } };

    for (const glm::ivec3& p : order) {
        Block b;
        if (!at(p, b) || !IsGrain(b)) continue;
        const BlockType type = b.type;
        int amount = Amount(b);
        const uint8_t water = b.WaterVolume();
        // Not upright: it settles so (keeping how much there is).
        if (!Upright(b)) {
            b = Flat(type, amount);
            if (water) b = b.WithWater(std::min<int>(water, b.IsPartial() ? b.WaterCapacity() : 0));
            set(p, b);
            moved.insert(p);
        }

        // 1. Fall.
        const glm::ivec3 down = p + glm::ivec3(0, -1, 0);
        Block under;
        if (at(down, under)) {
            if (open(under)) {
                set(down, Flat(type, amount));
                set(p, water ? Block::Water(water) : Block());
                moved.insert(down), moved.insert(p);
                continue;
            }
            if (under.IsLiquid()) {
                // Through water: the water comes up into the space it leaves.
                const int rising = under.WaterVolume() + water;
                if (rising <= Block::kMaxWaterVolume) {
                    set(down, Flat(type, amount));
                    set(p, Block::Water(static_cast<uint8_t>(rising)));
                    moved.insert(down), moved.insert(p);
                    continue;
                }
            }
            if (under.type == type && !under.HasWater() && Amount(under) < kFull) {
                const int below = Amount(under);
                int pour = std::min(amount, kFull - below);
                if (amount - pour > 0 && amount - pour < kMin) pour = amount - kMin; // (what stays is at least a pixel tall)
                if (pour > 0) {
                    set(down, Flat(type, below + pour));
                    amount -= pour;
                    set(p, amount > 0 ? Flat(type, amount) : (water ? Block::Water(water) : Block()));
                    moved.insert(down), moved.insert(p);
                    if (amount == 0) continue;
                    at(p, b);
                }
            }
        }
        if (water) continue; // (waterlogged: it only falls)

        // 2. Spread to the sides standing lower than it by more than it can stand.
        // (Their order turns from step to step, so piles don't lean one way.)
        const int repose = Repose(type);
        const int first = static_cast<int>((stepNumber + static_cast<uint32_t>(p.x * 7 + p.z * 13)) & 3);
        for (int s = 0; s < 4 && amount > 0; s++) {
            const glm::ivec3 n = p + kSides[(first + s) & 3];
            Block side;
            if (!at(n, side)) continue;
            int there = 0;   // what the side cell holds (of the same)
            int surface = 0; // its ground's height, relative to this cell's floor
            if (side.type == type && !side.HasWater()) {
                there = Amount(side);
                if (there >= kFull) continue;
                surface = there;
            }
            else if (open(side)) {
                Block below;
                if (!at(n + glm::ivec3(0, -1, 0), below)) continue;
                if (below.type == type && !below.HasWater()) surface = Amount(below) - kFull;
                else if (open(below) || below.IsLiquid()) surface = -kFull; // (a drop: it'll fall on from there)
                else surface = 0;
            }
            else continue; // (a wall: something else, or water)
            const int excess = amount - surface - repose;
            if (excess <= 0) continue;
            int share = (excess + 1) / 2;
            if (there == 0 && share < kMin) share = kMin;     // (a new cell is at least a pixel tall)
            share = std::min(share, kFull - there);
            if (amount - share > 0 && amount - share < kMin) share = amount; // (nor is what's left less)
            if (share <= 0 || share > amount || there + share > kFull) continue;
            amount -= share;
            set(n, Flat(type, there + share));
            set(p, amount > 0 ? Flat(type, amount) : Block());
            moved.insert(n), moved.insert(p);
        }
    }

    // 3. Shape: each cell left with some, its corners at the average height of the
    // cells round them (cells moved this step wait for the next: they'll be woken).
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> toShape(cells.begin(), cells.end());
    for (const glm::ivec3& p : toShape) {
        if (moved.count(p)) continue;
        Block b;
        if (!at(p, b) || !IsGrain(b) || b.HasWater() || b.IsFullBlock()) continue;
        const int amount = Amount(b);
        // A cell's ground height in this row (sixths of a sixteenth from its floor): of
        // the same, what it holds; open, the ground under it (or a drop); anything else
        // solid, full; water, as if level with this.
        auto height = [&](const glm::ivec3& q) {
            Block c;
            if (!at(q, c)) return amount;
            if (IsGrain(c) && !c.HasWater()) return Amount(c);
            if (open(c)) {
                Block below;
                if (!at(q + glm::ivec3(0, -1, 0), below) || below.IsLiquid()) return 0;
                if (IsGrain(below) && !below.HasWater()) return Amount(below) - kFull;
                return open(below) ? -kFull : 0;
            }
            if (c.IsLiquid() || c.HasWater()) return amount;
            return c.IsFullBlock() ? kFull : std::max(0, std::min(kFull, c.ResourceUnits() * kFull / Block::kUnitsPerBlock));
        };
        int h[3][3];
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++) h[dx + 1][dz + 1] = dx == 0 && dz == 0 ? amount : height(p + glm::ivec3(dx, 0, dz));
        // Corner (cx, cz) in 0..1: the four cells meeting there.
        auto corner = [&](int cx, int cz) {
            const int sum = h[cx][cz] + h[cx + 1][cz] + h[cx][cz + 1] + h[cx + 1][cz + 1];
            return (sum + 2 * kSixths) / (4 * kSixths); // in sixteenths
        };
        // EdgeData order: 0 = (x, z+1), 1 = (x+1, z), 2 = (x, z), 3 = (x+1, z+1).
        const Block shaped = Shape(b.type, amount, { corner(0, 1), corner(1, 0), corner(0, 0), corner(1, 1) });
        if (!(shaped == b)) set(p, shaped);
    }

    for (const auto& [p, b] : now) {
        Block was;
        if (!world.GetBlock(p.x, p.y, p.z, was) || !(was == b)) changes.push_back({ p, b });
    }
}

} // namespace Grains
