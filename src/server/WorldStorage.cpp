#include "server/WorldStorage.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>

#include "net/Protocol.h"

namespace fs = std::filesystem;

namespace {

constexpr char kRegionMagic[4] = { 'V', 'X', 'R', 'G' };
constexpr char kWorldMagic[4] = { 'V', 'X', 'W', 'D' };
constexpr size_t kMaxNameLength = 256;

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
    if (!r.VarU32(count)) count = 0;
    for (uint32_t i = 0; i < count; i++) {
        std::string name;
        PlayerRecord p;
        if (!r.Str(name, kMaxNameLength) || !r.Vec3(p.feet) || !r.F32(p.yaw) || !r.F32(p.pitch) || !r.U8(p.flags)) {
            std::cerr << "[storage] " << path.string() << " is truncated" << std::endl;
            return false;
        }
        if (std::isfinite(p.feet.x) && std::isfinite(p.feet.y) && std::isfinite(p.feet.z) && std::isfinite(p.yaw) && std::isfinite(p.pitch))
            players[name] = p;
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
        if (!net::DecodeBlockPlanes(planes, *blocks) || !planes.AtEnd()) {
            std::cerr << "[storage] chunk " << coords.x << "," << coords.y << "," << coords.z << " in " << path.string()
                      << " is corrupt" << std::endl;
            return false;
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
            w.VarU32(static_cast<uint32_t>(players.size()));
            for (const auto& [name, p] : players) {
                w.Str(name);
                w.Vec3(p.feet);
                w.F32(p.yaw);
                w.F32(p.pitch);
                w.U8(p.flags);
            }
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
