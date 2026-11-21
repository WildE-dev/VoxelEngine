#pragma once
// Creatures: what kinds there are and what they're like, shared by the server (which
// runs them, see server/Creatures.h) and the client (which draws them and aims at
// them).
//  - Deer wander grassland in herds and run from whoever hits them.
//  - Dwarves are the enemies, all of them: they live in camps on the surface and in
//    mines underground (world/Dwarves.h), guard them, and go for survival players
//    who come near. Dwarf warriors fight with axes; dwarf crossbowmen keep their
//    distance and shoot bolts; a dwarf chieftain, big and tough, holds each camp. On a
//    horde night a war band comes looking for players.
//  - Dwarf miners don't fight: hired at a mining post, they dig ore for you.
//  - Bats flutter about in the dark of caves, harmless.
//  - Wolves roam in pairs and leave you be unless you hit one. Fed raw venison, a wolf
//    is tamed: it follows its owner and fights the hostile dwarves near them.
#include <cstdint>
#include <string>

#include "world/Mining.h"

enum class CreatureType : uint8_t { Deer, DwarfWarrior, DwarfCrossbow, Wolf, Bat, DwarfChief, DwarfMiner, DwarfTrader, DwarfGuard, Count };
inline bool IsDwarf(CreatureType t) {
    return t == CreatureType::DwarfWarrior || t == CreatureType::DwarfCrossbow || t == CreatureType::DwarfChief || t == CreatureType::DwarfMiner ||
           t == CreatureType::DwarfTrader || t == CreatureType::DwarfGuard;
}
// The dwarves of a market (friendly, unless you hit one of them).
inline bool IsMarketDwarf(CreatureType t) { return t == CreatureType::DwarfTrader || t == CreatureType::DwarfGuard; }

struct CreatureSpec {
    const char* name;  // "Deer"
    const char* id;    // "deer", for commands
    float halfWidth;   // collision box: square footprint, half its side...
    float height;      // ...and its height
    int maxHealth;     // half hearts
    float walkSpeed;   // blocks per second, wandering
    float runSpeed;    // fleeing (deer) or chasing (ghouls)
    bool hostile;
    int attackDamage;  // half hearts per hit (hostile)
    float attackReach; // blocks, from the edge of its box
};

inline const CreatureSpec& SpecOf(CreatureType type) {
    static const CreatureSpec specs[] = {
        { "Deer", "deer", 0.45f, 1.3f, 8, 1.6f, 5.5f, false, 0, 0.0f },
        { "Dwarf warrior", "dwarf", 0.36f, 1.4f, 14, 1.1f, 3.3f, true, 3, 0.9f },
        { "Dwarf crossbowman", "crossbow_dwarf", 0.36f, 1.4f, 10, 1.1f, 2.9f, true, 0, 0.0f }, // (shoots instead)
        { "Wolf", "wolf", 0.32f, 0.85f, 16, 1.6f, 5.2f, false, 3, 0.8f },                    // (bites when provoked, or for its owner)
        { "Bat", "bat", 0.2f, 0.4f, 4, 2.5f, 4.0f, false, 0, 0.0f },                         // (flutters about caves)
        { "Dwarf chieftain", "dwarf_chief", 0.42f, 1.6f, 32, 1.0f, 3.0f, true, 5, 1.0f },
        { "Dwarf miner", "dwarf_miner", 0.36f, 1.4f, 14, 1.2f, 2.5f, false, 0, 0.0f },       // (works a mining post)
        { "Dwarf trader", "dwarf_trader", 0.38f, 1.4f, 20, 1.0f, 2.6f, false, 0, 0.0f },     // (trades at a market)
        { "Dwarf guard", "dwarf_guard", 0.38f, 1.45f, 24, 1.1f, 3.5f, false, 4, 1.0f },      // (guards a market, or whoever hired it)
    };
    const int t = static_cast<int>(type);
    return specs[t >= 0 && t < static_cast<int>(CreatureType::Count) ? t : 0];
}

inline CreatureType CreatureFromId(const std::string& id) {
    for (int t = 0; t < static_cast<int>(CreatureType::Count); t++)
        if (id == SpecOf(static_cast<CreatureType>(t)).id) return static_cast<CreatureType>(t);
    return CreatureType::Count;
}

namespace Combat {
// How far a player can hit (from the eye to the creature's box), how often, and how
// hard (half hearts) with each tool.
constexpr float kReach = 4.5f;
constexpr double kCooldown = 0.35;
inline int Damage(Tool tool) {
    switch (tool) {
    case Tool::Sword: return 7;
    case Tool::Axe: return 5;
    case Tool::Pickaxe: return 4;
    case Tool::Shovel: return 3;
    default: return 2;
    }
}
// With the iron upgrade, every tool hits a heart harder.
// `tier`: what the tools are made of (0 stone, 1 iron, 2 steel): 2 more for each.
inline int Damage(Tool tool, int tier) { return Damage(tool) + (tool != Tool::None ? 2 * tier : 0); }
} // namespace Combat
