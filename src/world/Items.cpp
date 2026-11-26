#include "world/Items.h"

#include <array>
#include <atomic>
#include <iostream>

#include "world/Json.h"

namespace {

constexpr size_t kCount = static_cast<size_t>(ItemType::COUNT);

struct Info {
    const char* id;
    const char* name;
    const char* texture;
};
// In ItemType order.
constexpr Info kInfo[kCount] = {
    { "none", "Nothing", nullptr },
    { "raw_venison", "Raw venison", "raw_venison" },
    { "cooked_venison", "Cooked venison", "cooked_venison" },
    { "raw_fish", "Raw fish", "raw_fish" },
    { "cooked_fish", "Cooked fish", "cooked_fish" },
    { "bread", "Bread", "bread" },
    { "seeds", "Seeds", "seeds" },
    { "wheat", "Wheat", "wheat" },
    { "hide", "Hide", "hide" },
    { "coal", "Coal", "coal" },
    { "coke", "Coke", "coke" },
    { "iron_ingot", "Iron ingot", "iron_ingot" },
    { "copper_ingot", "Copper ingot", "copper_ingot" },
    { "gold_ingot", "Gold ingot", "gold_ingot" },
    { "steel_ingot", "Steel ingot", "steel_ingot" },
    { "crushed_iron", "Crushed iron", "crushed_iron" },
    { "bitumen", "Bitumen", "bitumen" },
    { "cement", "Cement", "cement" },
    { "arrow", "Arrow", "arrow" },
    { "crossbow", "Crossbow", "crossbow" },
    { "boat", "Boat", "boat" },
    { "hammer", "Engineer's hammer", "hammer" },
    { "bucket", "Bucket", "bucket" },
    { "water_bucket", "Water bucket", "water_bucket" },
    { "forge_core", "Forge core", "forge_core" },
};
// Every type needs its entry: a missing one would read past the table's end.
static_assert(kInfo[kCount - 1].id != nullptr, "an item type is missing from kInfo");

struct Entry {
    std::optional<std::string> name, texture;
};
// The loaded table: a load puts in a new one and leaves the old be (see BlockData's).
std::atomic<const std::array<Entry, kCount>*>& Current() {
    static std::atomic<const std::array<Entry, kCount>*> current{ new std::array<Entry, kCount>() };
    return current;
}
const std::array<Entry, kCount>& Entries() { return *Current().load(std::memory_order_acquire); }

size_t Index(ItemType t) { return static_cast<size_t>(t) < kCount ? static_cast<size_t>(t) : 0; }

} // namespace

const char* ItemTypeId(ItemType type) { return kInfo[Index(type)].id; }

ItemType ItemTypeFromId(const std::string& id) {
    for (size_t t = 1; t < kCount; t++)
        if (id == kInfo[t].id) return static_cast<ItemType>(t);
    return ItemType::COUNT;
}

const char* DefaultItemName(ItemType type) { return kInfo[Index(type)].name; }
const char* DefaultItemTexture(ItemType type) { return kInfo[Index(type)].texture; }

namespace ItemData {

bool Load(const std::string& path, std::string& error) {
    Json::Value root;
    if (!Json::ParseDataPath(path, root, error)) return false;
    if (!root.IsObject()) {
        error = path + ": the file should be one { ... } of items";
        return false;
    }
    auto* loaded = new std::array<Entry, kCount>();
    for (const auto& [id, v] : root.object) {
        const ItemType t = ItemTypeFromId(id);
        if (t == ItemType::COUNT || !v.IsObject()) {
            std::cerr << "[data] " << path << ": no item called '" << id << "' (left out)" << std::endl;
            continue;
        }
        Entry& e = (*loaded)[static_cast<size_t>(t)];
        if (const auto* n = v.Find("name"); n && n->IsString()) e.name = n->string;
        if (const auto* tex = v.Find("texture"); tex && tex->IsString()) e.texture = tex->string;
    }
    Current().store(loaded, std::memory_order_release);
    return true;
}

std::optional<std::string> Name(ItemType t) { return Entries()[Index(t)].name; }
std::optional<std::string> Texture(ItemType t) { return Entries()[Index(t)].texture; }
const char* NameOf(ItemType t) {
    const auto& n = Entries()[Index(t)].name;
    return n ? n->c_str() : DefaultItemName(t);
}
const char* TextureOf(ItemType t) {
    const auto& s = Entries()[Index(t)].texture;
    return s ? s->c_str() : DefaultItemTexture(t);
}

} // namespace ItemData
