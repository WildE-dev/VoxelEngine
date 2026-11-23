#include "server/WorldStorage.h"

#include <algorithm>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>

#include "net/Protocol.h"
#include "world/Crafting.h"

namespace fs = std::filesystem;

namespace {

constexpr char kRegionMagic[4] = { 'V', 'X', 'R', 'G' };
constexpr char kWorldMagic[4] = { 'V', 'X', 'W', 'D' };
constexpr size_t kMaxNameLength = 256;
// Tagged player fields (format 7 on).
constexpr uint8_t kFieldHealth = 1, kFieldFood = 2, kFieldArmour = 3, kFieldBed = 4, kFieldAchievements = 5,
                  kFieldMoreTools = 6, // the tools mask's high byte (the chisel's bit, and after; before format 14)
                  kFieldInventory = 7; // their slots and offcuts (net::WriteInventory; format 14 on)

int FloorDiv(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

void WriteMagic(net::ByteWriter& w, const char (&magic)[4]) {
    for (char c : magic) w.U8(static_cast<uint8_t>(c));
}

bool ReadMagic(net::ByteReader& r, const char (&magic)[4]) {
    for (char c : magic) {
        uint8_t b;
        if (!r.U8(b) || b != static_cast<uint8_t>(c)) return false;
    }
    return true;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

WorldStorage::ColumnMask SkyMaskOf(const Chunk::Blocks& blocks) {
    constexpr int S = Chunk::CHUNK_SIZE;
    WorldStorage::ColumnMask mask{};
    for (int x = 0; x < S; x++)
        for (int z = 0; z < S; z++)
            for (int y = 0; y < S; y++) {
                const Block& b = blocks[Chunk::Index(x, y, z)];
                if (b.BlocksLight() || b.HasWater()) {
                    const int i = x * S + z;
                    mask[i / 64] |= uint64_t(1) << (i % 64);
                    break;
                }
            }
    return mask;
}

} // namespace

WorldStorage::WorldStorage(fs::path dir) : directory(std::move(dir)) {}

glm::ivec3 WorldStorage::RegionOf(const glm::ivec3& c) {
    return { FloorDiv(c.x, kRegionSize), FloorDiv(c.y, kRegionSize), FloorDiv(c.z, kRegionSize) };
}

fs::path WorldStorage::RegionPath(const glm::ivec3& r) const {
    return directory / "regions" / ("r." + std::to_string(r.x) + "." + std::to_string(r.y) + "." + std::to_string(r.z) + ".vxr");
}

bool WorldStorage::Open() {
    std::error_code ec;
    fs::create_directories(directory / "regions", ec);
    if (ec) {
        std::cerr << "[storage] can't create " << (directory / "regions").string() << ": " << ec.message() << std::endl;
        return false;
    }
    isNew = !fs::exists(directory / "world.dat");
    if (!isNew && !ReadWorldFile()) return false;

    for (const auto& entry : fs::directory_iterator(directory / "regions", ec)) {
        if (entry.path().extension() != ".vxr") continue;
        if (!ReadRegion(entry.path())) return false;
    }
    if (ec) {
        std::cerr << "[storage] can't list " << (directory / "regions").string() << ": " << ec.message() << std::endl;
        return false;
    }
    return true;
}

bool WorldStorage::ReadWorldFile() {
    const fs::path path = directory / "world.dat";
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) {
        std::cerr << "[storage] can't read " << path.string() << std::endl;
        return false;
    }
    net::ByteReader r(bytes.data(), bytes.size());
    uint32_t version = 0, count = 0;
    if (!ReadMagic(r, kWorldMagic) || !r.U32(version)) {
        std::cerr << "[storage] " << path.string() << " is not a world file" << std::endl;
        return false;
    }
    if (version > kFormatVersion) {
        std::cerr << "[storage] " << path.string() << " was saved by a newer version (format " << version << ")" << std::endl;
        return false;
    }
    if (version >= 2 && !r.U8(worldType)) {
        std::cerr << "[storage] " << path.string() << " is truncated" << std::endl;
        return false;
    }
    if (version >= 3 && (!r.F32(timeOfDay) || !std::isfinite(timeOfDay) || timeOfDay < 0.0f || timeOfDay >= 1.0f)) {
        std::cerr << "[storage] " << path.string() << " has a bad time of day" << std::endl;
        return false;
    }
    if (version >= 4 && (!r.U8(defaultMode) || defaultMode > 1)) {
        std::cerr << "[storage] " << path.string() << " has a bad game mode" << std::endl;
        return false;
    }
    if (!r.VarU32(count)) count = 0;
    for (uint32_t i = 0; i < count; i++) {
        std::string name;
        PlayerRecord p;
        if (!r.Str(name, kMaxNameLength) || !r.Vec3(p.feet) || !r.F32(p.yaw) || !r.F32(p.pitch) || !r.U8(p.flags)) {
            std::cerr << "[storage] " << path.string() << " is truncated" << std::endl;
            return false;
        }
        p.mode = defaultMode;
        if (version >= 4) {
            // (Before format 14: units by type and a tools mask, converted below.)
            std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)> oldUnits{};
            uint16_t oldTools = 0;
            bool hasInventory = false;
            uint32_t kinds = 0;
            bool ok = r.U8(p.mode) && p.mode <= 1 && r.VarU32(kinds) && kinds <= oldUnits.size();
            for (uint32_t k = 0; ok && k < kinds; k++) {
                uint8_t type;
                uint32_t units;
                ok = r.U8(type) && type < oldUnits.size() && r.VarU32(units);
                if (ok) oldUnits[type] = units;
            }
            uint8_t lowTools = 0;
            if (ok && version >= 6) ok = r.U8(lowTools), oldTools = lowTools;
            // Tagged fields: tag, length, then that many bytes.
            uint32_t fields = 0;
            if (ok && version >= 7) ok = r.VarU32(fields) && fields < 256;
            for (uint32_t f = 0; ok && f < fields; f++) {
                uint8_t tag;
                uint32_t length;
                ok = r.U8(tag) && r.VarU32(length) && length <= r.Remaining();
                if (!ok) break;
                uint8_t v;
                if (tag == kFieldHealth && length == 1) ok = r.U8(v), p.health = v;
                else if (tag == kFieldFood && length == 1) ok = r.U8(v), p.food = v;
                else if (tag == kFieldArmour && length == 1) ok = r.U8(v), p.armour = v;
                else if (tag == kFieldAchievements && length == 4) ok = r.U32(p.achievements);
                else if (tag == kFieldMoreTools && length == 1) ok = r.U8(v), oldTools = static_cast<uint16_t>((oldTools & 0xFF) | (v << 8));
                else if (tag == kFieldInventory) {
                    const size_t before = r.Remaining();
                    ok = net::ReadInventory(r, p.inventory) && before - r.Remaining() == length;
                    hasInventory = true;
                }
                else if (tag == kFieldBed && length == 12) {
                    glm::ivec3 bed;
                    ok = r.IVec3(bed);
                    p.bed = bed;
                }
                else ok = r.Skip(length);
            }
            if (!ok) {
                std::cerr << "[storage] " << path.string() << " is truncated" << std::endl;
                return false;
            }
            if (!hasInventory) p.inventory = FromOld(oldUnits, oldTools, Inventory::kPlayerSlots);
        }
        if (std::isfinite(p.feet.x) && std::isfinite(p.feet.y) && std::isfinite(p.feet.z) && std::isfinite(p.yaw) && std::isfinite(p.pitch))
            players[name] = p;
    }
    // Chests (format 8 on): position, then the kinds held and their units.
    uint32_t chestCount = 0;
    if (version >= 8 && !r.VarU32(chestCount)) chestCount = 0;
    for (uint32_t c = 0; c < chestCount; c++) {
        glm::ivec3 pos;
        Inventory held(Inventory::kChestSlots);
        bool ok = r.IVec3(pos);
        if (ok && version >= 14) ok = net::ReadInventory(r, held);
        else if (ok) {
            std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)> units{};
            uint32_t kinds = 0;
            ok = r.VarU32(kinds) && kinds <= units.size();
            for (uint32_t k = 0; ok && k < kinds; k++) {
                uint8_t type;
                ok = r.U8(type) && type < units.size() && r.VarU32(units[type]);
            }
            if (ok) held = FromOld(units, 0, Inventory::kChestSlots);
        }
        if (!ok) {
            std::cerr << "[storage] " << path.string() << " is truncated (chests)" << std::endl;
            return false;
        }
        chests.emplace_back(pos, held);
    }
    // Crops (format 9 on): positions.
    uint32_t cropCount = 0;
    if (version >= 9 && !r.VarU32(cropCount)) cropCount = 0;
    for (uint32_t c = 0; c < cropCount; c++) {
        glm::ivec3 pos;
        if (!r.IVec3(pos)) {
            std::cerr << "[storage] " << path.string() << " is truncated (crops)" << std::endl;
            return false;
        }
        crops.push_back(pos);
    }
    // Signs (format 10 on): position and text.
    uint32_t signCount = 0;
    if (version >= 10 && !r.VarU32(signCount)) signCount = 0;
    for (uint32_t i = 0; i < signCount; i++) {
        glm::ivec3 pos;
        std::string text;
        if (!r.IVec3(pos) || !r.Str(text, 256)) {
            std::cerr << "[storage] " << path.string() << " is truncated (signs)" << std::endl;
            return false;
        }
        signs.emplace_back(pos, text);
    }
    // Signal blocks (format 11 on).
    uint32_t signalCount = 0;
    if (version >= 11 && !r.VarU32(signalCount)) signalCount = 0;
    for (uint32_t i = 0; i < signalCount; i++) {
        SavedSignal sig;
        if (!r.IVec3(sig.pos) || !r.U8(sig.kind) || !r.U8(sig.mode) || !r.U8(sig.a) || !r.U8(sig.b) || !r.U8(sig.out) || !r.U8(sig.on)) {
            std::cerr << "[storage] " << path.string() << " is truncated (signals)" << std::endl;
            return false;
        }
        signalBlocks.push_back(sig);
    }
    // Fluids (format 12 on).
    uint32_t fluidCount = 0;
    if (version >= 12 && !r.VarU32(fluidCount)) fluidCount = 0;
    for (uint32_t i = 0; i < fluidCount; i++) {
        SavedFluid f;
        if (!r.IVec3(f.pos) || !r.U8(f.kind) || !r.VarU32(f.units)) {
            std::cerr << "[storage] " << path.string() << " is truncated (fluids)" << std::endl;
            return false;
        }
        fluids.push_back(f);
    }
    // Air (format 13 on).
    uint32_t airCount = 0;
    if (version >= 13 && !r.VarU32(airCount)) airCount = 0;
    for (uint32_t i = 0; i < airCount; i++) {
        SavedAir a;
        if (!r.IVec3(a.pos) || !r.F32(a.units)) {
            std::cerr << "[storage] " << path.string() << " is truncated (air)" << std::endl;
            return false;
        }
        if (std::isfinite(a.units) && a.units > 0.0f) air.push_back(a);
    }
    // Items on the ground (format 15 on).
    uint32_t itemCount = 0;
    if (version >= 15 && !r.VarU32(itemCount)) itemCount = 0;
    for (uint32_t i = 0; i < itemCount; i++) {
        SavedItem s;
        if (!r.U16(s.item) || !r.U8(s.count) || !r.U8(s.units) || !r.Vec3(s.position) || !r.F32(s.age)) {
            std::cerr << "[storage] " << path.string() << " is truncated (items)" << std::endl;
            return false;
        }
        if (Items::Valid(s.item) && std::isfinite(s.position.x) && std::isfinite(s.position.y) && std::isfinite(s.position.z) && std::isfinite(s.age))
            items.push_back(s);
    }
    // Leaves players put down (format 16 on).
    uint32_t leafCount = 0;
    if (version >= 16 && !r.VarU32(leafCount)) leafCount = 0;
    for (uint32_t i = 0; i < leafCount; i++) {
        glm::ivec3 pos;
        if (!r.IVec3(pos)) {
            std::cerr << "[storage] " << path.string() << " is truncated (placed leaves)" << std::endl;
            return false;
        }
        placedLeaves.push_back(pos);
    }
    return true;
}

bool WorldStorage::ReadRegion(const fs::path& path) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) {
        std::cerr << "[storage] can't read " << path.string() << std::endl;
        return false;
    }
    net::ByteReader r(bytes.data(), bytes.size());
    uint32_t version = 0, count = 0;
    if (!ReadMagic(r, kRegionMagic) || !r.U32(version) || version > kFormatVersion || !r.U32(count)) {
        std::cerr << "[storage] " << path.string() << " is not a region file this version can read" << std::endl;
        return false;
    }
    auto blocks = std::make_unique<Chunk::Blocks>();
    std::lock_guard<std::mutex> lock(mutex);
    for (uint32_t i = 0; i < count; i++) {
        glm::ivec3 coords;
        uint32_t length;
        if (!r.IVec3(coords) || !r.VarU32(length) || length > r.Remaining()) {
            std::cerr << "[storage] " << path.string() << " is truncated" << std::endl;
            return false;
        }
        StoredChunk chunk;
        chunk.data.resize(length);
        for (uint32_t b = 0; b < length; b++) r.U8(chunk.data[b]);

        net::ByteReader planes(chunk.data.data(), chunk.data.size());
        if (!net::DecodeBlockPlanes(planes, *blocks, version >= 5) || !planes.AtEnd()) {
            std::cerr << "[storage] chunk " << coords.x << "," << coords.y << "," << coords.z << " in " << path.string()
                      << " is corrupt" << std::endl;
            return false;
        }
        if (version < 5) {
            // Saved before blocks had an axis: kept in the current format from now on.
            net::ByteWriter current;
            net::EncodeBlockPlanes(current, *blocks);
            chunk.data = std::move(current.data);
        }
        chunk.skyBlocked = SkyMaskOf(*blocks);
        Insert(coords, std::move(chunk));
    }
    return true;
}

void WorldStorage::Insert(const glm::ivec3& coords, StoredChunk chunk) {
    columns[glm::ivec2(coords.x, coords.z)][coords.y] = chunk.skyBlocked;
    chunks[coords] = std::move(chunk);
}

bool WorldStorage::LoadChunk(const glm::ivec3& coords, Chunk::Blocks& out) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = chunks.find(coords);
    if (it == chunks.end()) return false;
    net::ByteReader in(it->second.data.data(), it->second.data.size());
    return net::DecodeBlockPlanes(in, out); // validated when stored or read
}

bool WorldStorage::HasChunk(const glm::ivec3& coords) const {
    std::lock_guard<std::mutex> lock(mutex);
    return chunks.count(coords) != 0;
}

void WorldStorage::StoreChunk(const glm::ivec3& coords, const Chunk::Blocks& blocks) {
    StoredChunk chunk;
    net::ByteWriter w;
    net::EncodeBlockPlanes(w, blocks);
    chunk.data = std::move(w.data);
    chunk.skyBlocked = SkyMaskOf(blocks);

    std::lock_guard<std::mutex> lock(mutex);
    Insert(coords, std::move(chunk));
    dirtyRegions.insert(RegionOf(coords));
}

void WorldStorage::SkyBlockedAbove(const glm::ivec3& coords, ColumnMask& out) const {
    out = {};
    std::lock_guard<std::mutex> lock(mutex);
    auto it = columns.find(glm::ivec2(coords.x, coords.z));
    if (it == columns.end()) return;
    for (auto y = it->second.upper_bound(coords.y); y != it->second.end(); ++y)
        for (size_t i = 0; i < out.size(); i++) out[i] |= y->second[i];
}

std::optional<WorldStorage::PlayerRecord> WorldStorage::FindPlayer(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = players.find(name);
    if (it == players.end()) return std::nullopt;
    return it->second;
}

void WorldStorage::StorePlayer(const std::string& name, const PlayerRecord& record) {
    std::lock_guard<std::mutex> lock(mutex);
    players[name.substr(0, kMaxNameLength)] = record;
    playersDirty = true;
}

uint8_t WorldStorage::GetWorldType() const {
    std::lock_guard<std::mutex> lock(mutex);
    return worldType;
}

void WorldStorage::SetWorldType(uint8_t type) {
    std::lock_guard<std::mutex> lock(mutex);
    worldType = type;
    playersDirty = true; // world.dat needs writing
}

uint8_t WorldStorage::GetDefaultMode() const {
    std::lock_guard<std::mutex> lock(mutex);
    return defaultMode;
}

void WorldStorage::SetDefaultMode(uint8_t mode) {
    std::lock_guard<std::mutex> lock(mutex);
    defaultMode = mode;
    playersDirty = true;
}

float WorldStorage::GetTimeOfDay() const {
    std::lock_guard<std::mutex> lock(mutex);
    return timeOfDay;
}

void WorldStorage::SetTimeOfDay(float t) {
    std::lock_guard<std::mutex> lock(mutex);
    timeOfDay = t;
    playersDirty = true; // world.dat needs writing
}

Inventory WorldStorage::FromOld(const std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)>& units, uint16_t tools, size_t slots) {
    Inventory inv(slots);
    const int tier = (tools & Crafting::kOldSteelBit) ? 2 : (tools & Crafting::kOldIronBit) ? 1 : 0;
    for (int t = 1; t < static_cast<int>(Tool::Count); t++)
        if (tools & Crafting::OldToolBit(static_cast<Tool>(t))) inv.Add(Items::OfTool(static_cast<Tool>(t), tier), 1, true);
    for (size_t t = 1; t < units.size(); t++)
        if (units[t] > 0) inv.AddUnits(static_cast<BlockType>(t), units[t], true);
    return inv;
}

WorldStorage::Chests WorldStorage::GetChests() const {
    std::lock_guard<std::mutex> lock(mutex);
    return chests;
}

void WorldStorage::SetChests(Chests c) {
    std::lock_guard<std::mutex> lock(mutex);
    chests = std::move(c);
    playersDirty = true; // world.dat needs writing
}

WorldStorage::Signs WorldStorage::GetSigns() const {
    std::lock_guard<std::mutex> lock(mutex);
    return signs;
}

std::vector<WorldStorage::SavedSignal> WorldStorage::GetSignals() const {
    std::lock_guard<std::mutex> lock(mutex);
    return signalBlocks;
}

void WorldStorage::SetSignals(std::vector<SavedSignal> s) {
    std::lock_guard<std::mutex> lock(mutex);
    signalBlocks = std::move(s);
    playersDirty = true;
}

std::vector<WorldStorage::SavedFluid> WorldStorage::GetFluids() const {
    std::lock_guard<std::mutex> lock(mutex);
    return fluids;
}

void WorldStorage::SetFluids(std::vector<SavedFluid> f) {
    std::lock_guard<std::mutex> lock(mutex);
    fluids = std::move(f);
    playersDirty = true;
}

std::vector<WorldStorage::SavedAir> WorldStorage::GetAir() const {
    std::lock_guard<std::mutex> lock(mutex);
    return air;
}

void WorldStorage::SetAir(std::vector<SavedAir> a) {
    std::lock_guard<std::mutex> lock(mutex);
    air = std::move(a);
    playersDirty = true;
}

std::vector<WorldStorage::SavedItem> WorldStorage::GetItems() const {
    std::lock_guard<std::mutex> lock(mutex);
    return items;
}

void WorldStorage::SetItems(std::vector<SavedItem> i) {
    std::lock_guard<std::mutex> lock(mutex);
    items = std::move(i);
    playersDirty = true;
}

void WorldStorage::SetSigns(Signs s) {
    std::lock_guard<std::mutex> lock(mutex);
    signs = std::move(s);
    playersDirty = true;
}

std::vector<glm::ivec3> WorldStorage::GetCrops() const {
    std::lock_guard<std::mutex> lock(mutex);
    return crops;
}

void WorldStorage::SetCrops(std::vector<glm::ivec3> c) {
    std::lock_guard<std::mutex> lock(mutex);
    crops = std::move(c);
    playersDirty = true;
}

std::vector<glm::ivec3> WorldStorage::GetPlacedLeaves() const {
    std::lock_guard<std::mutex> lock(mutex);
    return placedLeaves;
}

void WorldStorage::SetPlacedLeaves(std::vector<glm::ivec3> l) {
    std::lock_guard<std::mutex> lock(mutex);
    placedLeaves = std::move(l);
    playersDirty = true;
}

size_t WorldStorage::GetStoredChunkCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return chunks.size();
}

bool WorldStorage::WriteFileAtomically(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) return false;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

bool WorldStorage::Flush() {
    // Serialize under the lock, write without it so loads aren't held up by the disk.
    std::vector<std::pair<fs::path, std::vector<uint8_t>>> files;
    std::vector<glm::ivec3> regions;
    bool writePlayers = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (playersDirty || !fs::exists(directory / "world.dat")) {
            net::ByteWriter w;
            WriteMagic(w, kWorldMagic);
            w.U32(kFormatVersion);
            w.U8(worldType);
            w.F32(timeOfDay);
            w.U8(defaultMode);
            w.VarU32(static_cast<uint32_t>(players.size()));
            for (const auto& [name, p] : players) {
                w.Str(name);
                w.Vec3(p.feet);
                w.F32(p.yaw);
                w.F32(p.pitch);
                w.U8(p.flags);
                w.U8(p.mode);
                w.VarU32(0); // (units by type, before format 14)
                w.U8(0);     // (the tools mask, before format 14)
                w.VarU32(p.bed ? 6 : 5);
                w.U8(kFieldHealth), w.VarU32(1), w.U8(static_cast<uint8_t>(std::clamp(p.health, 0, 255)));
                w.U8(kFieldFood), w.VarU32(1), w.U8(static_cast<uint8_t>(std::clamp(p.food, 0, 255)));
                w.U8(kFieldArmour), w.VarU32(1), w.U8(p.armour);
                if (p.bed) w.U8(kFieldBed), w.VarU32(12), w.IVec3(*p.bed);
                w.U8(kFieldAchievements), w.VarU32(4), w.U32(p.achievements);
                net::ByteWriter items;
                net::WriteInventory(items, p.inventory);
                w.U8(kFieldInventory), w.VarU32(static_cast<uint32_t>(items.data.size())), w.Bytes(items.data.data(), items.data.size());
            }
            w.VarU32(static_cast<uint32_t>(chests.size()));
            for (const auto& [pos, held] : chests) {
                w.IVec3(pos);
                net::WriteInventory(w, held);
            }
            w.VarU32(static_cast<uint32_t>(crops.size()));
            for (const auto& pos : crops) w.IVec3(pos);
            w.VarU32(static_cast<uint32_t>(signs.size()));
            for (const auto& [pos, text] : signs) {
                w.IVec3(pos);
                w.Str(text);
            }
            w.VarU32(static_cast<uint32_t>(signalBlocks.size()));
            for (const auto& sig : signalBlocks) {
                w.IVec3(sig.pos);
                for (const uint8_t v : { sig.kind, sig.mode, sig.a, sig.b, sig.out, sig.on }) w.U8(v);
            }
            w.VarU32(static_cast<uint32_t>(fluids.size()));
            for (const auto& f : fluids) {
                w.IVec3(f.pos);
                w.U8(f.kind);
                w.VarU32(f.units);
            }
            w.VarU32(static_cast<uint32_t>(air.size()));
            for (const auto& a : air) {
                w.IVec3(a.pos);
                w.F32(a.units);
            }
            w.VarU32(static_cast<uint32_t>(items.size()));
            for (const auto& i : items) {
                w.U16(i.item);
                w.U8(i.count);
                w.U8(i.units);
                w.Vec3(i.position);
                w.F32(i.age);
            }
            w.VarU32(static_cast<uint32_t>(placedLeaves.size()));
            for (const auto& pos : placedLeaves) w.IVec3(pos);
            files.emplace_back(directory / "world.dat", std::move(w.data));
            writePlayers = true;
            playersDirty = false;
        }

        for (const auto& region : dirtyRegions) {
            const glm::ivec3 lo = region * kRegionSize;
            std::vector<const std::pair<const glm::ivec3, StoredChunk>*> entries;
            // A region has at most kRegionSize^3 chunks; scanning them beats scanning
            // every stored chunk once the save gets large.
            for (int x = 0; x < kRegionSize; x++)
                for (int y = 0; y < kRegionSize; y++)
                    for (int z = 0; z < kRegionSize; z++) {
                        auto it = chunks.find(lo + glm::ivec3(x, y, z));
                        if (it != chunks.end()) entries.push_back(&*it);
                    }
            net::ByteWriter w;
            WriteMagic(w, kRegionMagic);
            w.U32(kFormatVersion);
            w.U32(static_cast<uint32_t>(entries.size()));
            for (const auto* entry : entries) {
                w.IVec3(entry->first);
                w.VarU32(static_cast<uint32_t>(entry->second.data.size()));
                w.Bytes(entry->second.data.data(), entry->second.data.size());
            }
            files.emplace_back(RegionPath(region), std::move(w.data));
            regions.push_back(region);
        }
        dirtyRegions.clear();
    }

    bool ok = true;
    for (size_t i = 0; i < files.size(); i++) {
        if (WriteFileAtomically(files[i].first, files[i].second)) continue;
        std::cerr << "[storage] can't write " << files[i].first.string() << std::endl;
        ok = false;
        // Try again next time.
        std::lock_guard<std::mutex> lock(mutex);
        const size_t regionIndex = writePlayers ? i - 1 : i;
        if (writePlayers && i == 0) playersDirty = true;
        else dirtyRegions.insert(regions[regionIndex]);
    }
    return ok;
}
