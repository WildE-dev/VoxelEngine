#pragma once
// Inventories (survival): a grid of slots, each a stack of one kind of item, plus
// "offcuts" of each block type. Players, chests and machines all hold an Inventory.
//
// Items (ItemId): every block type is an item (its BlockType's number), and so is every
// tool in each of its three tiers (stone, iron, steel) and every piece of armour.
// Blocks stack to kMaxStack; tools and armour don't stack.
//
// Slots hold whole items only. Shaped blocks (slabs, slopes, chiselled steps) are worth
// their volume in units (Block::ResourceUnits, 64 to a block): placing one takes its
// units from the type's offcut, cutting a whole item into it first if the offcut is
// short; breaking or chiselling one adds its volume to the offcut, and every full 64
// becomes an item in a slot again. So an offcut is always less than a block, and the
// total of a type in units (Units) is items * 64 + offcut. Recipes may take part of a
// block the same way. Offcuts stay where they are: only whole items move between slots,
// chests and the hotbar.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "world/Armour.h"
#include "world/Block.h"
#include "world/Mining.h"

using ItemId = uint16_t;

namespace Items {

constexpr ItemId kNone = 0; // (BlockType::AIR)
constexpr ItemId kToolBase = 256;   // + tool * 4 + tier
constexpr ItemId kArmourBase = 512; // + piece
constexpr int kTiers = 3;           // stone, iron, steel
constexpr uint32_t kMaxStack = 64;
constexpr uint32_t kUnits = Block::kUnitsPerBlock;

inline ItemId OfBlock(BlockType t) { return static_cast<ItemId>(t); }
inline ItemId OfTool(Tool tool, int tier = 0) { return static_cast<ItemId>(kToolBase + static_cast<unsigned>(tool) * 4 + std::clamp(tier, 0, kTiers - 1)); }
inline ItemId OfArmour(ArmourPiece p) { return static_cast<ItemId>(kArmourBase + static_cast<unsigned>(p)); }

inline bool IsBlock(ItemId id) { return id > 0 && id < static_cast<ItemId>(BlockType::COUNT); }
inline bool IsTool(ItemId id) {
    if (id < kToolBase || id >= kArmourBase) return false;
    const unsigned t = (id - kToolBase) / 4, tier = (id - kToolBase) % 4;
    return t > 0 && t < static_cast<unsigned>(Tool::Count) && tier < static_cast<unsigned>(kTiers);
}
inline bool IsArmour(ItemId id) { return id >= kArmourBase && id < kArmourBase + static_cast<unsigned>(ArmourPiece::Count); }
inline bool Valid(ItemId id) { return IsBlock(id) || IsTool(id) || IsArmour(id); }

inline BlockType BlockOf(ItemId id) { return IsBlock(id) ? static_cast<BlockType>(id) : BlockType::AIR; }
inline Tool ToolOf(ItemId id) { return IsTool(id) ? static_cast<Tool>((id - kToolBase) / 4) : Tool::None; }
inline int TierOf(ItemId id) { return IsTool(id) ? (id - kToolBase) % 4 : 0; }
inline ArmourPiece ArmourOf(ItemId id) { return IsArmour(id) ? static_cast<ArmourPiece>(id - kArmourBase) : ArmourPiece::Count; }

inline uint32_t MaxStack(ItemId id) {
    if (id == static_cast<ItemId>(BlockType::BUCKET) || id == static_cast<ItemId>(BlockType::WATER_BUCKET)) return 1; // (one each)
    return IsBlock(id) ? kMaxStack : 1;
}

inline const char* TierName(int tier) { return tier >= 2 ? "Steel" : tier == 1 ? "Iron" : "Stone"; }

// For commands: a block's id ("stone"), a tool's ("pickaxe", "iron_pickaxe",
// "steel_pickaxe"), a piece of armour's ("cap"). And back (kNone: no such thing).
inline std::string Id(ItemId id) {
    if (IsBlock(id)) return BlockId(BlockOf(id));
    if (IsTool(id)) {
        const int tier = TierOf(id);
        return std::string(tier == 2 ? "steel_" : tier == 1 ? "iron_" : "") + Mining::ToolId(ToolOf(id));
    }
    if (IsArmour(id)) return Armour::Id(ArmourOf(id));
    return "?";
}
inline ItemId FromId(const std::string& id) {
    int tier = 0;
    std::string rest = id;
    if (rest.rfind("steel_", 0) == 0) tier = 2, rest = rest.substr(6);
    else if (rest.rfind("iron_", 0) == 0) tier = 1, rest = rest.substr(5);
    if (const Tool t = Mining::ToolFromId(rest); t != Tool::None) return OfTool(t, tier);
    if (const ArmourPiece p = Armour::FromId(id); p != ArmourPiece::Count) return OfArmour(p);
    const BlockType b = Current(BlockFromId(id)); // ("pipe": a fluid pipe now)
    return b == BlockType::COUNT || b == BlockType::AIR ? kNone : OfBlock(b);
}
// For people: "Iron pickaxe", "Hide cap"; a block's is its id made readable.
inline std::string Name(ItemId id) {
    if (IsTool(id)) {
        std::string tool = Mining::ToolName(ToolOf(id));
        tool[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(tool[0])));
        return std::string(TierName(TierOf(id))) + " " + tool;
    }
    if (IsArmour(id)) return Armour::Name(ArmourOf(id));
    std::string s = Id(id);
    for (char& c : s)
        if (c == '_') c = ' ';
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

} // namespace Items

struct Stack {
    ItemId item = Items::kNone;
    uint8_t count = 0;
    bool Empty() const { return count == 0 || item == Items::kNone; }
    bool operator==(const Stack& o) const { return (Empty() && o.Empty()) || (item == o.item && count == o.count); }
};

class Inventory {
public:
    static constexpr size_t kChestSlots = 30; // three rows of ten
    // A player's: the hotbar (keys 1-9 and 0) first, then the rest.
    static constexpr size_t kHotbarSlots = 10;
    static constexpr size_t kPlayerSlots = kHotbarSlots + 30;
    static constexpr size_t kTypes = static_cast<size_t>(BlockType::COUNT);

    explicit Inventory(size_t slots = kChestSlots) : slots(slots) {}

    std::vector<Stack> slots;
    std::array<uint8_t, kTypes> offcuts{}; // units of each type, always less than a block

    bool operator==(const Inventory& o) const { return slots == o.slots && offcuts == o.offcuts; }

    // Whole items.
    uint32_t Count(ItemId item) const {
        uint32_t n = 0;
        for (const Stack& s : slots)
            if (!s.Empty() && s.item == item) n += s.count;
        return n;
    }
    // How many more of `item` would fit.
    uint32_t Room(ItemId item) const {
        if (!Items::Valid(item)) return 0;
        const uint32_t max = Items::MaxStack(item);
        uint32_t n = 0;
        for (const Stack& s : slots) n += s.Empty() ? max : s.item == item ? max - std::min<uint32_t>(max, s.count) : 0;
        return n;
    }
    // Adds up to `n`, into stacks of it first, then empty slots in order (`grow`: adds
    // slots for whatever doesn't fit). Returns how many didn't fit.
    uint32_t Add(ItemId item, uint32_t n, bool grow = false) {
        if (!Items::Valid(item)) return n;
        const uint32_t max = Items::MaxStack(item);
        for (Stack& s : slots) {
            if (n == 0) break;
            if (s.Empty() || s.item != item || s.count >= max) continue;
            const uint32_t k = std::min(n, max - s.count);
            s.count = static_cast<uint8_t>(s.count + k), n -= k;
        }
        for (Stack& s : slots) {
            if (n == 0) break;
            if (!s.Empty()) continue;
            const uint32_t k = std::min(n, max);
            s = { item, static_cast<uint8_t>(k) }, n -= k;
        }
        while (grow && n > 0) {
            const uint32_t k = std::min(n, max);
            slots.push_back({ item, static_cast<uint8_t>(k) }), n -= k;
        }
        return n;
    }
    // Takes `n` (from the last stacks of it first), or nothing if there aren't that many.
    bool Take(ItemId item, uint32_t n) {
        if (Count(item) < n) return false;
        for (size_t i = slots.size(); i-- > 0 && n > 0;) {
            Stack& s = slots[i];
            if (s.Empty() || s.item != item) continue;
            const uint32_t k = std::min<uint32_t>(n, s.count);
            s.count = static_cast<uint8_t>(s.count - k), n -= k;
            if (s.count == 0) s = {};
        }
        return true;
    }

    // Units of a block type: whole items and the offcut.
    uint32_t Units(BlockType t) const {
        if (t == BlockType::AIR || t >= BlockType::COUNT) return 0;
        return Count(Items::OfBlock(t)) * Items::kUnits + offcuts[static_cast<size_t>(t)];
    }
    // Whether `units` more of a type would fit.
    bool FitsUnits(BlockType t, uint32_t units) const {
        if (t == BlockType::AIR || t >= BlockType::COUNT) return false;
        return (offcuts[static_cast<size_t>(t)] + units) / Items::kUnits <= Room(Items::OfBlock(t));
    }
    // Adds units of a type (whole blocks into slots, the rest to the offcut). Returns the
    // units that didn't fit (whole blocks' worth).
    uint32_t AddUnits(BlockType t, uint32_t units, bool grow = false) {
        if (t == BlockType::AIR || t >= BlockType::COUNT || units == 0) return units;
        const uint32_t total = offcuts[static_cast<size_t>(t)] + units;
        offcuts[static_cast<size_t>(t)] = static_cast<uint8_t>(total % Items::kUnits);
        return Add(Items::OfBlock(t), total / Items::kUnits, grow) * Items::kUnits;
    }
    // Takes units of a type (from the offcut, cutting whole items as needed), or nothing
    // if there aren't that many.
    bool TakeUnits(BlockType t, uint32_t units) {
        if (units == 0) return true;
        if (Units(t) < units) return false;
        uint8_t& off = offcuts[static_cast<size_t>(t)];
        if (off >= units) {
            off = static_cast<uint8_t>(off - units);
            return true;
        }
        const uint32_t needed = units - off;
        const uint32_t cut = (needed + Items::kUnits - 1) / Items::kUnits;
        Take(Items::OfBlock(t), cut);
        off = static_cast<uint8_t>(cut * Items::kUnits - needed);
        return true;
    }

    // Tools: whether there's one of this kind, and the best tier of it (-1: none).
    int BestTier(Tool tool) const {
        int best = -1;
        for (const Stack& s : slots)
            if (!s.Empty() && Items::ToolOf(s.item) == tool) best = std::max(best, Items::TierOf(s.item));
        return best;
    }

    bool Empty() const {
        for (const Stack& s : slots)
            if (!s.Empty()) return false;
        for (uint8_t o : offcuts)
            if (o) return false;
        return true;
    }
    void Clear() {
        for (Stack& s : slots) s = {};
        offcuts.fill(0);
    }
    // Shrinks back toward `size` slots, dropping empty ones off the end (a chest that
    // grew to take an old world's contents).
    void Trim(size_t size) {
        while (slots.size() > size && slots.back().Empty()) slots.pop_back();
    }
};
