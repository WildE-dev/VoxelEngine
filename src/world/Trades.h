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

#include "world/Inventory.h"

namespace Trades {

constexpr double kHireSeconds = 600.0;
constexpr int kMaxHired = 2;        // guards hired by one player at once
constexpr float kReach = 6.0f;      // how near the trader you must be
constexpr double kPriceRecovery = 60.0;
constexpr int kMaxPressure = 12;    // (at most four times the price)

struct Offer {
    ItemRef give;  // what you give (a block or item type)...
    int giveCount; // ...how many (whole ones), to start with
    ItemRef get;   // what you get (none with `hire`: a guard)
    int getCount = 0;
    bool hire = false;
};

inline const std::vector<Offer>& Offers() {
    static const std::vector<Offer> offers = {
        // What they sell, for gold.
        { ItemType::GOLD_INGOT, 1, ItemType::IRON_INGOT, 2 },
        { ItemType::GOLD_INGOT, 2, ItemType::STEEL_INGOT, 1 },
        { ItemType::GOLD_INGOT, 1, ItemType::BREAD, 6 },
        { ItemType::GOLD_INGOT, 1, ItemType::ARROW, 12 },
        { ItemType::GOLD_INGOT, 2, BlockType::TNT, 2 },
        // What they buy, for gold.
        { BlockType::GLASS, 8, ItemType::GOLD_INGOT, 1 },
        { ItemType::STEEL_INGOT, 2, ItemType::GOLD_INGOT, 1 },
        { ItemType::CRUSHED_IRON, 8, ItemType::GOLD_INGOT, 1 },
        { ItemType::COKE, 4, ItemType::GOLD_INGOT, 1 },
        { ItemType::COAL, 16, ItemType::GOLD_INGOT, 1 },
        { ItemType::HIDE, 4, ItemType::GOLD_INGOT, 1 },
        // Barter.
        { ItemType::BREAD, 4, ItemType::IRON_INGOT, 1 },
        { ItemType::IRON_INGOT, 3, ItemType::STEEL_INGOT, 1 },
        { ItemType::GOLD_INGOT, 1, ItemType::COPPER_INGOT, 4 },
        // A guard, for ten minutes (last: tests take it by being last).
        { ItemType::GOLD_INGOT, 3, Items::kNone, 0, true },
    };
    return offers;
}

// What an offer costs with its price pushed up `pressure` times.
inline int Price(const Offer& o, int pressure) { return o.giveCount + (o.giveCount * std::clamp(pressure, 0, kMaxPressure) + 3) / 4; }

} // namespace Trades
