#include "world/BlockData.h"

#include <array>
#include <cstdio>
#include <iostream>

#include "world/Json.h"
#include "world/Mining.h"

namespace BlockData {

int8_t gLight[static_cast<size_t>(BlockType::COUNT)];

namespace {

constexpr size_t kCount = static_cast<size_t>(BlockType::COUNT);
struct Entry {
    std::optional<std::string> name;
    std::array<std::optional<std::string>, 6> textures;
    std::optional<float> hardness;
    std::optional<Tool> tool;
    std::optional<std::string> sound;
    std::optional<uint32_t> map;
};
std::array<Entry, kCount> entries;

struct LightInit {
    LightInit() {
        for (auto& l : gLight) l = -1;
    }
} lightInit;

std::optional<Tool> ToolFrom(const std::string& s) {
    if (s == "pickaxe") return Tool::Pickaxe;
    if (s == "axe") return Tool::Axe;
    if (s == "shovel") return Tool::Shovel;
    if (s == "sword") return Tool::Sword;
    if (s == "none") return Tool::None;
    return std::nullopt;
}

} // namespace

bool Load(const std::string& path, std::string& error) {
    Json::Value root;
    if (!Json::ParseFile(path, root, error)) return false;
    if (!root.IsObject()) {
        error = path + ": the file should be one { ... } of blocks";
        return false;
    }
    std::array<Entry, kCount> loaded;
    int8_t light[kCount];
    for (auto& l : light) l = -1;
    for (const auto& [id, v] : root.object) {
        const BlockType t = id == "air" ? BlockType::AIR : BlockFromId(id);
        if (t == BlockType::COUNT || !v.IsObject()) {
            std::cerr << "[data] " << path << ": no block called '" << id << "' (left out)" << std::endl;
            continue;
        }
        Entry& e = loaded[static_cast<size_t>(t)];
        if (const auto* n = v.Find("name"); n && n->IsString()) e.name = n->string;
        if (const auto* tex = v.Find("textures"); tex && tex->IsObject()) {
            const std::string all = tex->StringOr("all", ""), side = tex->StringOr("side", all);
            const char* keys[6] = { "top", "bottom", "front", "back", "left", "right" };
            for (int f = 0; f < 6; f++) {
                const std::string fallback = f < 2 ? all : side;
                const std::string name = tex->StringOr(keys[f], fallback);
                if (!name.empty()) e.textures[f] = name;
            }
        }
        if (const auto* h = v.Find("hardness"); h && h->IsNumber()) e.hardness = static_cast<float>(h->number);
        if (const auto* tool = v.Find("tool"); tool && tool->IsString()) {
            e.tool = ToolFrom(tool->string);
            if (!e.tool) std::cerr << "[data] " << path << ": " << id << ": no tool called '" << tool->string << "'" << std::endl;
        }
        if (const auto* snd = v.Find("sound"); snd && snd->IsString()) e.sound = snd->string;
        if (const auto* l = v.Find("light"); l && l->IsNumber()) light[static_cast<size_t>(t)] = static_cast<int8_t>(std::max(0.0, std::min(15.0, l->number)));
        if (const auto* m = v.Find("map"); m && m->IsString() && m->string.size() == 7 && m->string[0] == '#') {
            const uint32_t rgb = static_cast<uint32_t>(std::strtoul(m->string.c_str() + 1, nullptr, 16));
            e.map = 0xFF000000u | ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16);
        }
    }
    entries = std::move(loaded);
    for (size_t i = 0; i < kCount; i++) gLight[i] = light[i];
    return true;
}

const char* NameOr(BlockType t, const char* fallback) {
    const auto& n = entries[static_cast<size_t>(t) % kCount].name;
    return n ? n->c_str() : fallback;
}
const char* SoundOr(BlockType t, const char* fallback) {
    const auto& s = entries[static_cast<size_t>(t) % kCount].sound;
    return s ? s->c_str() : fallback;
}

std::optional<std::string> Name(BlockType t) { return entries[static_cast<size_t>(t) % kCount].name; }
std::optional<std::string> Texture(BlockType t, int face) { return entries[static_cast<size_t>(t) % kCount].textures[static_cast<size_t>(face) % 6]; }
std::optional<float> Hardness(BlockType t) { return entries[static_cast<size_t>(t) % kCount].hardness; }
std::optional<Tool> BestTool(BlockType t) { return entries[static_cast<size_t>(t) % kCount].tool; }
std::optional<std::string> Sound(BlockType t) { return entries[static_cast<size_t>(t) % kCount].sound; }
std::optional<uint32_t> MapColour(BlockType t) { return entries[static_cast<size_t>(t) % kCount].map; }

} // namespace BlockData
