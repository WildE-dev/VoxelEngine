#pragma once
// Achievements: milestones a player earns once each (the server awards them, tells
// everyone, and saves them with the player; /achievements lists them). Earning one
// pops up a toast on the player's screen (chat lines starting with kToastMark).
#include <cstdint>

enum class Achievement : uint8_t {
    FirstBlock,     // break a block
    Toolmaker,      // make a tool
    Hunter,         // kill a deer
    DwarfSlayer,    // defeat a hostile dwarf
    Cook,           // cook venison
    FirstMeal,      // eat something
    Farmer,         // harvest ripe wheat
    Baker,          // bake bread
    Miner,          // dig out iron ore
    IronAge,        // make the iron upgrade
    Armoured,       // wear a full set of armour
    TreasureHunter, // open a chest in a dungeon
    SweetDreams,    // sleep through a night
    Marksman,       // kill something with an arrow
    Angler,         // catch a fish
    BestFriend,     // tame a wolf
    Count
};

namespace Achievements {

// Chat lines that start with this are achievement toasts.
constexpr const char* kToastMark = "[Achievement] ";

inline uint32_t Bit(Achievement a) { return 1u << static_cast<unsigned>(a); }

inline const char* Name(Achievement a) {
    switch (a) {
    case Achievement::FirstBlock: return "Breaking Ground";
    case Achievement::Toolmaker: return "Toolmaker";
    case Achievement::Hunter: return "Hunter";
    case Achievement::DwarfSlayer: return "Dwarf Slayer";
    case Achievement::Cook: return "Cook";
    case Achievement::FirstMeal: return "First Meal";
    case Achievement::Farmer: return "Farmer";
    case Achievement::Baker: return "Baker";
    case Achievement::Miner: return "Iron in the Rock";
    case Achievement::IronAge: return "Iron Age";
    case Achievement::Armoured: return "Suited Up";
    case Achievement::TreasureHunter: return "Treasure Hunter";
    case Achievement::SweetDreams: return "Sweet Dreams";
    case Achievement::Marksman: return "Marksman";
    case Achievement::Angler: return "Angler";
    case Achievement::BestFriend: return "Best Friend";
    default: return "?";
    }
}

inline const char* Description(Achievement a) {
    switch (a) {
    case Achievement::FirstBlock: return "break your first block";
    case Achievement::Toolmaker: return "make a tool";
    case Achievement::Hunter: return "bring down a deer";
    case Achievement::DwarfSlayer: return "defeat a hostile dwarf";
    case Achievement::Cook: return "cook some venison";
    case Achievement::FirstMeal: return "eat something";
    case Achievement::Farmer: return "harvest ripe wheat";
    case Achievement::Baker: return "bake bread";
    case Achievement::Miner: return "dig out iron ore";
    case Achievement::IronAge: return "make the iron upgrade";
    case Achievement::Armoured: return "wear a cap, a tunic and boots";
    case Achievement::TreasureHunter: return "open a chest in a dungeon";
    case Achievement::SweetDreams: return "sleep through the night";
    case Achievement::Marksman: return "take something down with an arrow";
    case Achievement::Angler: return "catch a fish";
    case Achievement::BestFriend: return "tame a wolf with some venison";
    default: return "";
    }
}

} // namespace Achievements
