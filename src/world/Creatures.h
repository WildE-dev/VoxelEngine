#pragma once
// Creatures: what kinds there are and what they're like, shared by the server (which
// runs them, see server/Creatures.h) and the client (which draws them and aims at
// them).
//  - Deer wander grassland in herds and run from whoever hits them.
//  - Ghouls come out in the dark (caves, dungeons, the surface at night) and go for
//    survival players nearby. Daylight burns them.
#include <cstdint>
#include <string>

#include "world/Mining.h"

enum class CreatureType : uint8_t { Deer, Ghoul, Count };

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
        { "Ghoul", "ghoul", 0.3f, 1.8f, 12, 1.2f, 3.4f, true, 3, 0.9f },
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
inline int Damage(Tool tool, bool iron) { return Damage(tool) + (iron && tool != Tool::None ? 2 : 0); }
} // namespace Combat
