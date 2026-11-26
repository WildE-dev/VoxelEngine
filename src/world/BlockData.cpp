#include "world/BlockData.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <iostream>

#include "world/Items.h"
#include "world/Json.h"
#include "world/Mining.h"

namespace BlockData {

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
// What a load set. The server reads it on its own threads while the game may load the
// files again, so a load puts in a new table and leaves the old one be (never freed:
// loads are rare and small), as Crafting::Current does.
struct Table {
    std::array<Entry, kCount> entries;
    std::array<Shape, kCount> shapes;
    std::array<int8_t, kCount> light;
    Table() { light.fill(-1); }
};
std::atomic<const Table*>& Current() {
    static std::atomic<const Table*> current{ new Table() };
    return current;
}
const Table& T() { return *Current().load(std::memory_order_acquire); }

constexpr std::array<int8_t, kCount> NoLights() {
    std::array<int8_t, kCount> l{};
    for (auto& v : l) v = -1;
    return l;
}
constexpr std::array<int8_t, kCount> kNoLights = NoLights();

std::optional<Tool> ToolFrom(const std::string& s) {
    if (s == "pickaxe") return Tool::Pickaxe;
    if (s == "axe") return Tool::Axe;
    if (s == "shovel") return Tool::Shovel;
    if (s == "sword") return Tool::Sword;
    if (s == "none") return Tool::None;
    return std::nullopt;
}

// "+x" .. "-z": the face's bit (0 if it isn't one).
uint8_t FaceBit(const std::string& s) {
    static const char* names[kFaceCount] = { "+x", "-x", "+y", "-y", "+z", "-z" };
    for (int f = 0; f < kFaceCount; f++)
        if (s == names[f]) return static_cast<uint8_t>(1u << f);
    return 0;
}

bool Vec3(const Json::Value* v, glm::vec3& out) {
    if (!v || !v->IsArray() || v->array.size() != 3) return false;
    for (int i = 0; i < 3; i++) {
        if (!v->array[static_cast<size_t>(i)].IsNumber()) return false;
        out[i] = static_cast<float>(v->array[static_cast<size_t>(i)].number);
    }
    return true;
}

// A block's "collision", but for its references to other blocks' boxes (left in
// `refs`, by where they go in the list).
struct RawShape {
    Shape shape;
    std::vector<std::pair<size_t, BlockType>> refs;
};

RawShape ReadShape(const std::string& path, const std::string& id, const Json::Value& c) {
    RawShape raw;
    Shape& s = raw.shape;
    auto warn = [&](const std::string& what) { std::cerr << "[data] " << path << ": " << id << ": collision: " << what << std::endl; };
    s.turns = c.BoolOr("turns", false);
    s.mirrors = c.BoolOr("mirrors", false);
    if (const auto* v = c.Find("solid"); v && v->type == Json::Value::Type::Bool) s.solid = v->boolean;
    if (const auto* v = c.Find("floor"); v && v->type == Json::Value::Type::Bool) s.floor = v->boolean;
    const Json::Value* boxes = c.Find("boxes");
    if (!boxes) return raw;
    std::vector<const Json::Value*> list;
    if (boxes->IsArray())
        for (const auto& b : boxes->array) list.push_back(&b);
    else list.push_back(boxes);
    s.hasBoxes = true;
    for (const Json::Value* b : list) {
        if (b->IsString()) {
            const BlockType other = BlockFromId(b->string);
            if (other == BlockType::COUNT) warn("no block called '" + b->string + "' to take boxes from");
            else raw.refs.emplace_back(s.boxes.size(), other);
            continue;
        }
        ShapeBox box;
        if (!b->IsObject() || !Vec3(b->Find("from"), box.lo) || !Vec3(b->Find("to"), box.hi)) {
            warn("a box needs \"from\" and \"to\", each [x, y, z]");
            continue;
        }
        const glm::vec3 from = box.lo, to = box.hi; // (either way round)
        box.lo = glm::clamp(glm::min(from, to), 0.0f, 1.0f);
        box.hi = glm::clamp(glm::max(from, to), 0.0f, 1.0f);
        auto faces = [&](const char* key) {
            uint8_t bits = 0;
            if (const auto* f = b->Find(key); f && f->IsArray())
                for (const auto& name : f->array) {
                    const uint8_t bit = name.IsString() ? FaceBit(name.string) : 0;
                    if (!bit) warn(std::string(key) + ": faces are \"+x\", \"-x\", \"+y\", \"-y\", \"+z\" or \"-z\"");
                    bits |= bit;
                }
            return bits;
        };
        box.joinedAll = faces("joined");
        box.joinedAny = faces("joinedAny");
        box.joinedNone = faces("joinedNone");
        box.otherwise = b->BoolOr("otherwise", false);
        s.boxes.push_back(box);
    }
    return raw;
}

// The boxes of `t`, with its references followed (not round in a circle).
std::vector<ShapeBox> Resolve(std::array<RawShape, kCount>& raw, BlockType t, int depth) {
    const RawShape& r = raw[static_cast<size_t>(t)];
    if (depth > 8) return r.shape.boxes;
    std::vector<ShapeBox> out;
    size_t ref = 0;
    for (size_t i = 0; i <= r.shape.boxes.size(); i++) {
        for (; ref < r.refs.size() && r.refs[ref].first == i; ref++) {
            const auto more = Resolve(raw, r.refs[ref].second, depth + 1);
            out.insert(out.end(), more.begin(), more.end());
        }
        if (i < r.shape.boxes.size()) out.push_back(r.shape.boxes[i]);
    }
    return out;
}

} // namespace

std::atomic<const int8_t*> gLight{ kNoLights.data() };

bool Load(const std::string& path, std::string& error) {
    Json::Value root;
    if (!Json::ParseDataPath(path, root, error)) return false;
    if (!root.IsObject()) {
        error = path + ": the file should be one { ... } of blocks";
        return false;
    }
    std::array<Entry, kCount> loaded;
    int8_t light[kCount];
    for (auto& l : light) l = -1;
    std::array<RawShape, kCount> rawShapes;
    for (const auto& [id, v] : root.object) {
        const BlockType t = id == "air" ? BlockType::AIR : BlockFromId(id);
        if (t == BlockType::COUNT || !v.IsObject()) {
            // (An item's file left from when items were blocks: they're in Resources/Data/items now.)
            const bool item = ItemTypeFromId(id) != ItemType::COUNT;
            std::cerr << "[data] " << path << ": no block called '" << id << "'" << (item ? " (an item now: see Resources/Data/items)" : " (left out)") << std::endl;
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
        if (const auto* c = v.Find("collision"); c && c->IsObject()) rawShapes[static_cast<size_t>(t)] = ReadShape(path, id, *c);
    }
    std::array<Shape, kCount> resolved;
    for (size_t i = 0; i < kCount; i++) {
        resolved[i] = rawShapes[i].shape;
        if (!rawShapes[i].refs.empty()) resolved[i].boxes = Resolve(rawShapes, static_cast<BlockType>(i), 0);
    }
    auto* table = new Table();
    table->entries = std::move(loaded);
    table->shapes = std::move(resolved);
    for (size_t i = 0; i < kCount; i++) table->light[i] = light[i];
    Current().store(table, std::memory_order_release);
    gLight.store(table->light.data(), std::memory_order_release);
    return true;
}

const char* NameOr(BlockType t, const char* fallback) {
    const auto& n = T().entries[static_cast<size_t>(t) % kCount].name;
    return n ? n->c_str() : fallback;
}
const char* SoundOr(BlockType t, const char* fallback) {
    const auto& s = T().entries[static_cast<size_t>(t) % kCount].sound;
    return s ? s->c_str() : fallback;
}

const Shape& ShapeOf(BlockType t) { return T().shapes[static_cast<size_t>(t) % kCount]; }

std::optional<std::string> Name(BlockType t) { return T().entries[static_cast<size_t>(t) % kCount].name; }
std::optional<std::string> Texture(BlockType t, int face) { return T().entries[static_cast<size_t>(t) % kCount].textures[static_cast<size_t>(face) % 6]; }
std::optional<float> Hardness(BlockType t) { return T().entries[static_cast<size_t>(t) % kCount].hardness; }
std::optional<Tool> BestTool(BlockType t) { return T().entries[static_cast<size_t>(t) % kCount].tool; }
std::optional<std::string> Sound(BlockType t) { return T().entries[static_cast<size_t>(t) % kCount].sound; }
std::optional<uint32_t> MapColour(BlockType t) { return T().entries[static_cast<size_t>(t) % kCount].map; }

} // namespace BlockData
