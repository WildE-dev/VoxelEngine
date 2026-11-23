#pragma once
// Armour (survival): pieces made from deer hide (see world/Crafting.h), each made once
// and then always worn, like tools are kept. Together they cut the damage you take
// from hits (creatures, other players) and traps; not from drowning, thin or stale air,
// or starving. The server applies it (Server::Hurt).
// A shield (planks and iron) and a steel plate (steel over hide, worn over the tunic)
// are armour too. Crouching with a shield blocks: half of a hit's damage, after armour.
// An oxygen tank (worn on the back) protects nothing: it holds air to breathe (see
// world/Breathing.h).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

enum class ArmourPiece : uint8_t { Cap, Tunic, Boots, Shield, Plate, OxygenTank, Count };
constexpr float kMaxProtection = 0.8f;

namespace Armour {

// Worn pieces, one bit each.
inline uint8_t Bit(ArmourPiece piece) { return static_cast<uint8_t>(1u << static_cast<unsigned>(piece)); }

inline const char* Name(ArmourPiece piece) {
    switch (piece) {
    case ArmourPiece::Cap: return "Hide cap";
    case ArmourPiece::Tunic: return "Hide tunic";
    case ArmourPiece::Boots: return "Hide boots";
    case ArmourPiece::Shield: return "Shield";
    case ArmourPiece::Plate: return "Steel plate";
    case ArmourPiece::OxygenTank: return "Oxygen tank";
    default: return "?";
    }
}
inline const char* Id(ArmourPiece piece) {
    switch (piece) {
    case ArmourPiece::Cap: return "cap";
    case ArmourPiece::Tunic: return "tunic";
    case ArmourPiece::Boots: return "boots";
    case ArmourPiece::Shield: return "shield";
    case ArmourPiece::Plate: return "plate";
    case ArmourPiece::OxygenTank: return "tank";
    default: return "?";
    }
}
// The piece's icon (a texture in Resources/Textures/Blocks).
inline const char* Texture(ArmourPiece piece) {
    switch (piece) {
    case ArmourPiece::Cap: return "armour_cap";
    case ArmourPiece::Tunic: return "armour_tunic";
    case ArmourPiece::Boots: return "armour_boots";
    case ArmourPiece::Shield: return "armour_shield";
    case ArmourPiece::Plate: return "armour_plate";
    case ArmourPiece::OxygenTank: return "armour_tank";
    default: return nullptr;
    }
}
inline ArmourPiece FromId(const std::string& id) {
    for (int p = 0; p < static_cast<int>(ArmourPiece::Count); p++)
        if (id == Id(static_cast<ArmourPiece>(p))) return static_cast<ArmourPiece>(p);
    return ArmourPiece::Count;
}

// The share of damage a piece takes off.
inline float Protection(ArmourPiece piece) {
    switch (piece) {
    case ArmourPiece::Cap: return 0.12f;
    case ArmourPiece::Tunic: return 0.24f;
    case ArmourPiece::Boots: return 0.12f;
    case ArmourPiece::Shield: return 0.12f;
    case ArmourPiece::Plate: return 0.25f;
    default: return 0.0f;
    }
}
// The share of damage all the worn pieces (`worn`, Bit each) take off.
inline float Protection(uint8_t worn) {
    float total = 0.0f;
    for (int p = 0; p < static_cast<int>(ArmourPiece::Count); p++)
        if (worn & Bit(static_cast<ArmourPiece>(p))) total += Protection(static_cast<ArmourPiece>(p));
    return std::min(total, kMaxProtection);
}
// Damage (half hearts) after armour: always at least one if there was any.
inline int Reduce(int damage, uint8_t worn) {
    if (damage <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(damage * (1.0f - Protection(worn)))));
}

} // namespace Armour
