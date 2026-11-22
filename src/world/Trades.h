#pragma once
// What a dwarf trader at a market (world/Dwarves.h) deals in. Gold (gold ingots, from
// the Deep) is their money: they sell things for it, buy things you make for it, and
// still barter a little. One offer hires a dwarf guard, who follows you and fights
// hostile dwarves for kHireSeconds.
//
// Prices drift: each time an offer is taken its price (what you give) goes up by a
// quarter of what it starts at, and falls back a step every kPriceRecovery seconds.
// Sell a lot of glass and they'll want more of it for their gold; buy up their steel
// and it costs more, for a while. (The server keeps the prices, for every market at
// once, and tells a player them when they open a trader.) A market whose dwarves
// you've hit won't trade with you for a while (see server/Creatures.h).
#include <algorithm>
#include <vector>

#include "world/Block.h"

namespace Trades {

constexpr double kHireSeconds = 600.0;
constexpr int kMaxHired = 2;        // guards hired by one player at once
constexpr float kReach = 6.0f;      // how near the trader you must be
constexpr double kPriceRecovery = 60.0;
constexpr int kMaxPressure = 12;    // (at most four times the price)

struct Offer {
    BlockType give;                 // what you give...
    int giveCount;                  // ...how many (whole items or blocks), to start with
    BlockType get = BlockType::AIR; // what you get (AIR with `hire`: a guard)
    int getCount = 0;
    bool hire = false;
};

inline const std::vector<Offer>& Offers() {
    static const std::vector<Offer> offers = {
        // What they sell, for gold.
        { BlockType::GOLD_INGOT, 1, BlockType::IRON_INGOT, 2 },
        { BlockType::GOLD_INGOT, 2, BlockType::STEEL_INGOT, 1 },
        { BlockType::GOLD_INGOT, 1, BlockType::BREAD, 6 },
        { BlockType::GOLD_INGOT, 1, BlockType::ARROW, 12 },
        { BlockType::GOLD_INGOT, 2, BlockType::TNT, 2 },
        // What they buy, for gold.
        { BlockType::GLASS, 8, BlockType::GOLD_INGOT, 1 },
        { BlockType::STEEL_INGOT, 2, BlockType::GOLD_INGOT, 1 },
        { BlockType::CRUSHED_IRON, 8, BlockType::GOLD_INGOT, 1 },
        { BlockType::COKE, 4, BlockType::GOLD_INGOT, 1 },
        { BlockType::COAL, 16, BlockType::GOLD_INGOT, 1 },
        { BlockType::HIDE, 4, BlockType::GOLD_INGOT, 1 },
        // Barter.
        { BlockType::BREAD, 4, BlockType::IRON_INGOT, 1 },
        { BlockType::IRON_INGOT, 3, BlockType::STEEL_INGOT, 1 },
        { BlockType::GOLD_INGOT, 1, BlockType::COPPER_INGOT, 4 },
        // A guard, for ten minutes (last: tests take it by being last).
        { BlockType::GOLD_INGOT, 3, BlockType::AIR, 0, true },
    };
    return offers;
}

// What an offer costs with its price pushed up `pressure` times.
inline int Price(const Offer& o, int pressure) { return o.giveCount + (o.giveCount * std::clamp(pressure, 0, kMaxPressure) + 3) / 4; }

} // namespace Trades
