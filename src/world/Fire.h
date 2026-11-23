#pragma once
// Fire: a real danger. A fire block sits in an open cell beside something that burns.
// The server runs it (Server::UpdateFire, every kInterval):
//
//  - It eats what it touches: each burnable block beside a fire goes, sooner or later
//    (BurnRate: leaves and plants in a moment, planks in a few seconds, logs slowly),
//    and often the fire takes its place.
//  - It spreads: it tries the cells round it (higher ones more: fire climbs), and catches
//    in an open one beside something that burns (as readily as that burns: Flammability).
//    A forest goes up fast. Rain slows it but doesn't stop it.
//  - With nothing left to burn it dies down in a few seconds.
//  - It's hard to put out: it can't be punched out (it isn't aimed at). Water does it:
//    water flowing into a fire's cell puts it out, and a bucket of water splashed on one
//    puts out the fires round it too. So does a block put where it is (smothering it:
//    sand, dirt, stone), and taking away what it would burn next (a firebreak).
//  - It burns: a player in it catches fire and goes on burning for kBurnSeconds after
//    stepping out, unless they get into water; creatures in it are hurt; items lying in it
//    are lost; TNT beside it is set off.
//  - Lava sets fire to what's beside it, lightning to where it strikes, and a TNT blast to
//    what's round it.
#include "world/Block.h"

namespace Fire {

constexpr double kInterval = 0.25;   // seconds between steps
constexpr float kDieOut = 3.0f;      // seconds a fire with nothing to burn lasts (and up to as long again)
constexpr float kSpreadTries = 3.0f; // cells round each fire tried, a second
constexpr float kRainDamping = 0.3f; // how much of its spread a fire keeps in the rain (open sky)
constexpr double kBurnSeconds = 4.0; // a player stays alight this long after leaving the fire
constexpr int kBurnDamage = 1;       // half a heart
constexpr double kBurnHurtInterval = 0.5;
constexpr size_t kMaxFires = 4000;

// How readily a block catches (0: never): what a fire beside it spreads by.
inline float Flammability(BlockType t) {
    switch (t) {
    case BlockType::LEAVES:
    case BlockType::PINE_LEAVES:
    case BlockType::TALL_GRASS:
    case BlockType::FLOWER_RED:
    case BlockType::FLOWER_YELLOW:
    case BlockType::DEAD_BUSH:
    case BlockType::CROP:
    case BlockType::RIPE_WHEAT:
    case BlockType::SAPLING:
    case BlockType::PINE_SAPLING: return 1.0f;
    case BlockType::BED: return 0.8f;
    case BlockType::PLANKS:
    case BlockType::DOOR:
    case BlockType::DOOR_OPEN:
    case BlockType::LADDER:
    case BlockType::SIGN: return 0.6f;
    case BlockType::CHEST:
    case BlockType::ITEM_PIPE:
    case BlockType::FILTER_PIPE:
    case BlockType::CRAFTER: return 0.4f;
    case BlockType::LOG: return 0.35f;
    case BlockType::CACTUS: return 0.2f;
    default: return 0.0f;
    }
}
inline bool Burns(BlockType t) { return Flammability(t) > 0.0f; }
// How fast a burnable block beside a fire is eaten: the chance of it going, a second.
inline float BurnRate(BlockType t) {
    if (IsPlantType(t)) return 1.5f;
    if (IsLeavesType(t)) return 0.35f;
    if (t == BlockType::LOG) return 0.05f;
    if (t == BlockType::CACTUS) return 0.08f;
    return 0.15f;
}
// Whether a fire can be in this cell: open (air, or a plant it burns up), dry.
inline bool CanBurnIn(const Block& b) { return (b.IsAir() || (b.IsPlant() && b.type != BlockType::FIRE && Burns(b.type))) && !b.HasWater(); }

} // namespace Fire
