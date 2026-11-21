#pragma once
// What a dwarf trader at a market (world/Dwarves.h) trades: each offer takes some of
// one thing from you and gives some of another, or hires a dwarf guard, who follows you
// and fights hostile dwarves for kHireSeconds. A market whose dwarves you've hit won't
// trade with you for a while (see server/Creatures.h).
#include <vector>

#include "world/Block.h"

namespace Trades {

constexpr double kHireSeconds = 600.0;
constexpr int kMaxHired = 2;        // guards hired by one player at once
constexpr float kReach = 6.0f;      // how near the trader you must be

struct Offer {
    BlockType give;                 // what you give...
    int giveCount;                  // ...how many (whole items or blocks)
    BlockType get = BlockType::AIR; // what you get (AIR with `hire`: a guard)
    int getCount = 0;
    bool hire = false;
};

inline const std::vector<Offer>& Offers() {
    static const std::vector<Offer> offers = {
        { BlockType::BREAD, 4, BlockType::IRON_INGOT, 1 },
        { BlockType::COAL, 8, BlockType::IRON_INGOT, 1 },
        { BlockType::IRON_INGOT, 3, BlockType::STEEL_INGOT, 1 },
        { BlockType::HIDE, 2, BlockType::ARROW, 8 },
        { BlockType::IRON_INGOT, 2, BlockType::TNT, 2 },
        { BlockType::STEEL_INGOT, 1, BlockType::BREAD, 6 },
        { BlockType::IRON_INGOT, 4, BlockType::AIR, 0, true },
    };
    return offers;
}

} // namespace Trades
