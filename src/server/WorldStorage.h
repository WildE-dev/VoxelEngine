#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "world/Chunk.h"
#include "world/Coords.h"
#include "world/Inventory.h"

// A saved world on disk. Terrain generation is deterministic, so only chunks that were
// edited are stored; everything else is generated again when it loads. Light isn't
// stored either: it's recomputed when a chunk loads.
//
// Layout of the world directory:
//   world.dat            format version, world type and the players (position and
//                        look, by name)
//   regions/r.X.Y.Z.vxr  the stored chunks of one region (kRegionSize^3 chunks), each
//                        as RLE block planes (see net::EncodeBlockPlanes)
//
// Every file is written to a temporary file and renamed over the old one, so a crash
// mid-save leaves the previous save intact.
//
// All stored chunks are kept in memory (compressed, typically 1-3 KB each for edited
// surface chunks) so lookups never touch the disk. Thread safe.
class WorldStorage
{
public:
    static constexpr int kRegionSize = 8;      // chunks per region along each axis
    // 2: world type; 3: time of day; 4: game modes and resources; 5: block axes (logs);
    // 6: tools players have made; ...; 14: inventories of item stacks (world/Inventory.h)
    // in place of units by type, tool and upgrade masks (older ones are converted)
    static constexpr uint32_t kFormatVersion = 14;

    struct PlayerRecord {
        glm::vec3 feet{0.0f};
        float yaw = -90.0f, pitch = 0.0f;
        uint8_t flags = 0; // net::kFlagFlying etc.
        uint8_t mode = 0;  // net::GameMode
        Inventory inventory{ Inventory::kPlayerSlots }; // what they carry
        // From format 7, each saved as a tagged field (unknown tags are skipped, so
        // more can be added without breaking older saves).
        int health = 20; // half hearts
        int food = 20;   // world/Hunger.h
        uint8_t armour = 0; // armour worn (Armour::Bit each)
        std::optional<glm::ivec3> bed; // where they respawn
        uint32_t achievements = 0;     // Achievements::Bit each
    };

    // Chests' contents, by position (format 8 on; units by block type before format 14,
    // converted into stacks when read).
    using ChestItems = Inventory;
    using Chests = std::vector<std::pair<glm::ivec3, ChestItems>>;
    Chests GetChests() const;
    void SetChests(Chests chests); // saved with the next Flush
    // Crops still growing, by position (format 9 on).
    std::vector<glm::ivec3> GetCrops() const;
    void SetCrops(std::vector<glm::ivec3> crops);
    // Signs' texts, by position (format 10 on).
    using Signs = std::vector<std::pair<glm::ivec3, std::string>>;
    Signs GetSigns() const;
    void SetSigns(Signs signs);
    // Signal blocks' channels (world/Signals.h), by position (format 11 on): kind, gate
    // mode, inputs, output, and whether it's on (a gate's output, a sluice gate open).
    struct SavedSignal {
        glm::ivec3 pos{0};
        uint8_t kind = 0, mode = 0, a = 0, b = 0, out = 0, on = 0;
    };
    std::vector<SavedSignal> GetSignals() const;
    void SetSignals(std::vector<SavedSignal> s);
    // What fluid pipes and tanks hold (world/Fluids.h), by position (format 12 on).
    struct SavedFluid {
        glm::ivec3 pos{0};
        uint8_t kind = 0;
        uint32_t units = 0;
    };
    std::vector<SavedFluid> GetFluids() const;
    void SetFluids(std::vector<SavedFluid> f);
    // The air held in air tanks, pipes and machines (world/Power.h), by position
    // (format 13 on; before that, air networks started empty).
    struct SavedAir {
        glm::ivec3 pos{0};
        float units = 0.0f;
    };
    std::vector<SavedAir> GetAir() const;
    void SetAir(std::vector<SavedAir> a);

    // One bit per block column of a chunk (index x * CHUNK_SIZE + z).
    using ColumnMask = std::array<uint64_t, Chunk::CHUNK_SIZE * Chunk::CHUNK_SIZE / 64>;

    explicit WorldStorage(std::filesystem::path directory);
    // What an old save's player held (units by block type, the tools mask with its iron
    // and steel upgrade bits) as an inventory: each tool made at the upgrade's tier, then
    // the blocks (whole ones into slots, the rest offcuts). Nothing is lost: the slots
    // grow past `slots` if they have to.
    static Inventory FromOld(const std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)>& units, uint16_t tools, size_t slots);

    // Creates the directory if needed and reads everything saved in it. Returns false
    // (with a message on stderr) if the save can't be read.
    bool Open();
    const std::filesystem::path& GetDirectory() const { return directory; }
    // Whether Open found no saved world (world.dat) to read.
    bool IsNew() const { return isNew; }
    // Which generator the world uses (TerrainGenerator::Type); 0 for worlds saved
    // before it was recorded. Set it once for a new world.
    uint8_t GetWorldType() const;
    void SetWorldType(uint8_t worldType);
    // Time of day (0..1, see net::PlayerSnapshot), saved with the world. Worlds saved
    // before it was recorded start in the morning.
    float GetTimeOfDay() const;
    void SetTimeOfDay(float timeOfDay);
    // The game mode new players start in (net::GameMode). Worlds saved before modes
    // existed were played as creative.
    uint8_t GetDefaultMode() const;
    void SetDefaultMode(uint8_t mode);

    // The saved blocks of a chunk, if it was ever stored.
    bool LoadChunk(const glm::ivec3& coords, Chunk::Blocks& out) const;
    bool HasChunk(const glm::ivec3& coords) const;
    // Replaces the stored copy (written to disk on the next Flush).
    void StoreChunk(const glm::ivec3& coords, const Chunk::Blocks& blocks);

    // Block columns of a chunk that a stored chunk somewhere above it blocks sky light
    // in (anything opaque or holding water). Light for a chunk whose upper neighbour
    // isn't loaded is estimated from the generator, which only knows untouched terrain;
    // with this the estimate stays "never too bright" when the terrain above was built
    // on (see LightPropagator).
    void SkyBlockedAbove(const glm::ivec3& coords, ColumnMask& out) const;

    std::optional<PlayerRecord> FindPlayer(const std::string& name) const;
    void StorePlayer(const std::string& name, const PlayerRecord& record);

    // Writes everything changed since the last flush. Returns false on an I/O error
    // (with a message on stderr); what couldn't be written stays pending.
    bool Flush();

    size_t GetStoredChunkCount() const;

    static bool IsColumnSet(const ColumnMask& mask, int x, int z) {
        const int i = x * Chunk::CHUNK_SIZE + z;
        return (mask[i / 64] >> (i % 64)) & 1;
    }

private:
    struct StoredChunk {
        std::vector<uint8_t> data; // RLE block planes
        ColumnMask skyBlocked{};
    };
    struct IVec2Hash {
        size_t operator()(const glm::ivec2& v) const noexcept { return Coords::IVec3Hash{}(glm::ivec3(v, 0)); }
    };

    static glm::ivec3 RegionOf(const glm::ivec3& chunk);
    std::filesystem::path RegionPath(const glm::ivec3& region) const;
    bool ReadRegion(const std::filesystem::path& path);
    bool ReadWorldFile();
    void Insert(const glm::ivec3& coords, StoredChunk chunk); // caller holds `mutex`
    static bool WriteFileAtomically(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);

    const std::filesystem::path directory;

    mutable std::mutex mutex;
    std::unordered_map<glm::ivec3, StoredChunk, Coords::IVec3Hash> chunks;
    // Stored chunks by column, for SkyBlockedAbove: chunk y -> its sky mask.
    std::unordered_map<glm::ivec2, std::map<int, ColumnMask>, IVec2Hash> columns;
    std::unordered_map<std::string, PlayerRecord> players;
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> dirtyRegions;
    bool playersDirty = false;
    bool isNew = true;
    uint8_t worldType = 0;
    float timeOfDay = 0.05f;
    uint8_t defaultMode = 1; // creative, for worlds saved before modes
    Chests chests;
    std::vector<glm::ivec3> crops;
    Signs signs;
    std::vector<SavedSignal> signalBlocks;
    std::vector<SavedFluid> fluids;
    std::vector<SavedAir> air;
};
