#pragma once
// Felling trees, and leaves falling away from them (the server's rules; the client
// predicts the felling with the same function).
//
//  - Breaking a log with an axe fells it: every log straight above it, as far as the
//    column of logs goes (kFellLimit at most), comes down with it (each giving its worth,
//    in survival).
//  - Leaves need a log near them. When a log goes (broken, felled), leaves within
//    kLeafScan of it with no log within kLeafReach fall away one by one over the next few
//    seconds (Server::UpdateLeaves), now and then dropping a sapling.
#include <vector>

#include <glm/glm.hpp>

#include "world/BlockAccess.h"

namespace Trees {

constexpr int kFellLimit = 48;   // logs felled at once, at most
constexpr int kLeafReach = 4;    // leaves within this of a log (per axis) stay
constexpr int kLeafScan = 5;     // how far from a log that went to look for leaves left alone
constexpr double kLeafFallMin = 0.5, kLeafFallMax = 6.0; // seconds before each falls
constexpr float kSaplingChance = 0.08f;

inline bool IsLog(const Block& b) { return b.type == BlockType::LOG; }

// The logs an axe fells with the one at `pos`: those straight above it, as far as they go.
inline std::vector<glm::ivec3> LogsAbove(const BlockAccess& world, const glm::ivec3& pos) {
    std::vector<glm::ivec3> out;
    for (int y = pos.y + 1; static_cast<int>(out.size()) < kFellLimit; y++) {
        Block b;
        if (!world.GetBlock(pos.x, y, pos.z, b) || !IsLog(b)) break;
        out.emplace_back(pos.x, y, pos.z);
    }
    return out;
}

// Whether there's a log within kLeafReach of this cell (per axis).
inline bool LogNear(const BlockAccess& world, const glm::ivec3& cell) {
    for (int dx = -kLeafReach; dx <= kLeafReach; dx++)
        for (int dy = -kLeafReach; dy <= kLeafReach; dy++)
            for (int dz = -kLeafReach; dz <= kLeafReach; dz++) {
                Block b;
                if (world.GetBlock(cell.x + dx, cell.y + dy, cell.z + dz, b) && IsLog(b)) return true;
            }
    return false;
}

// Leaves around where logs went (within kLeafScan) that no log holds up any more.
inline std::vector<glm::ivec3> LooseLeaves(const BlockAccess& world, const std::vector<glm::ivec3>& gone) {
    std::vector<glm::ivec3> out;
    for (const auto& g : gone)
        for (int dx = -kLeafScan; dx <= kLeafScan; dx++)
            for (int dy = -kLeafScan; dy <= kLeafScan; dy++)
                for (int dz = -kLeafScan; dz <= kLeafScan; dz++) {
                    const glm::ivec3 c = g + glm::ivec3(dx, dy, dz);
                    Block b;
                    if (!world.GetBlock(c.x, c.y, c.z, b) || !IsLeavesType(b.type)) continue;
                    bool seen = false;
                    for (const auto& o : out) seen |= o == c;
                    if (!seen && !LogNear(world, c)) out.push_back(c);
                }
    return out;
}

} // namespace Trees
