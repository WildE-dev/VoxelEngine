#pragma once
// Armour (survival): pieces made from deer hide (see world/Crafting.h), items worn in
// armour slots of their own, one for each piece (world/Inventory.h). Together they cut
// the damage you take from hits (creatures, other players) and traps; not from
// drowning, thin or stale air, or starving. The server applies it (Server::Hurt).
// A shield (planks and iron) and a steel plate (steel over hide, worn over the tunic)
// are armour too. Crouching with a shield blocks: half of a hit's damage, after armour.
// An oxygen tank (worn on the back) protects nothing: it holds air to breathe (see
// world/Breathing.h).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

enum class ArmourPiece : uint8_t { Cap, Tunic, Boots, Shield, Plate, OxygenTank, Mask, Leggings, Count };
// Where a piece is worn: one piece in each of these at a time.
enum class BodySlot : uint8_t { Head, Face, Body, Back, Legs, Feet, Count };
constexpr float kMaxProtection = 0.8f;

namespace Armour {

inline BodySlot SlotOf(ArmourPiece piece) {
    switch (piece) {
    case ArmourPiece::Cap: return BodySlot::Head;
    case ArmourPiece::Mask: return BodySlot::Face;
    case ArmourPiece::Tunic:
    case ArmourPiece::Plate: return BodySlot::Body;
    case ArmourPiece::Shield:
    case ArmourPiece::OxygenTank: return BodySlot::Back; // (a shield slung there, raised by crouching)
    case ArmourPiece::Leggings: return BodySlot::Legs;
    case ArmourPiece::Boots: return BodySlot::Feet;
    default: return BodySlot::Count;
    }
}
inline const char* SlotName(BodySlot slot) {
    switch (slot) {
    case BodySlot::Head: return "Head";
    case BodySlot::Face: return "Face";
    case BodySlot::Body: return "Body";
    case BodySlot::Back: return "Back";
    case BodySlot::Legs: return "Legs";
    case BodySlot::Feet: return "Feet";
    default: return "?";
    }
}
// The piece an empty slot shows, faintly (what goes there).
inline ArmourPiece SlotIcon(BodySlot slot) {
    switch (slot) {
    case BodySlot::Head: return ArmourPiece::Cap;
    case BodySlot::Face: return ArmourPiece::Mask;
    case BodySlot::Body: return ArmourPiece::Tunic;
    case BodySlot::Back: return ArmourPiece::OxygenTank;
    case BodySlot::Legs: return ArmourPiece::Leggings;
    default: return ArmourPiece::Boots;
    }
}

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
    case ArmourPiece::Mask: return "Mask";
    case ArmourPiece::Leggings: return "Hide leggings";
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
    case ArmourPiece::Mask: return "mask";
    case ArmourPiece::Leggings: return "leggings";
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
    case ArmourPiece::Mask: return "armour_mask";
    case ArmourPiece::Leggings: return "armour_leggings";
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
    case ArmourPiece::Plate: return 0.4f; // (in place of a tunic, not over it)
    case ArmourPiece::Mask: return 0.04f; // (and it keeps steam out: no scalding)
    case ArmourPiece::Leggings: return 0.18f;
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
// The piece worn in a slot (Count: none).
inline ArmourPiece InSlot(uint8_t worn, BodySlot slot) {
    for (int p = 0; p < static_cast<int>(ArmourPiece::Count); p++)
        if ((worn & Bit(static_cast<ArmourPiece>(p))) && SlotOf(static_cast<ArmourPiece>(p)) == slot) return static_cast<ArmourPiece>(p);
    return ArmourPiece::Count;
}
// One piece to a slot (what an older save may have had more of): the one that
// protects most is kept on; the others come off into `off` (bits).
inline uint8_t OnePerSlot(uint8_t worn, uint8_t& off) {
    off = 0;
    for (int s = 0; s < static_cast<int>(BodySlot::Count); s++) {
        int keep = -1;
        for (int p = 0; p < static_cast<int>(ArmourPiece::Count); p++) {
            const auto piece = static_cast<ArmourPiece>(p);
            if (!(worn & Bit(piece)) || SlotOf(piece) != static_cast<BodySlot>(s)) continue;
            if (keep < 0 || Protection(piece) > Protection(static_cast<ArmourPiece>(keep))) {
                if (keep >= 0) off |= Bit(static_cast<ArmourPiece>(keep));
                keep = p;
            }
            else off |= Bit(piece);
        }
    }
    return static_cast<uint8_t>(worn & ~off);
}
// Damage (half hearts) after armour: always at least one if there was any.
inline int Reduce(int damage, uint8_t worn) {
    if (damage <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(damage * (1.0f - Protection(worn)))));
}

} // namespace Armour
