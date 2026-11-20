#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <glm/glm.hpp>

#include "world/Block.h"

class ChunkMesher;

// A CHUNK_SIZE^3 block of voxels: blocks and light. No GL here; the client wraps chunks
// in ClientChunk for meshes.
//
// Threading contract:
//  - Block data is guarded by an internal mutex and may be read/written from any thread.
//  - On the server, light is only ever read or written on the world worker thread (by
//    light propagation), so it needs no lock there. The worker publishes a copy
//    (PublishLight) that other threads read with CopyPublishedLight.
//  - On the client, light arrives from the network: SetLight writes it under the block
//    mutex, and the mesher reads it under the same mutex.
//  - The destructor may run on any thread.
class Chunk
{
public:
    // Vertex positions are packed as (pos * 8) into 8 bits, so 31 is the maximum.
    static constexpr int CHUNK_SIZE = 16;
    static constexpr int VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;
    using Blocks = std::array<Block, VOLUME>;
    using Light = std::array<uint8_t, VOLUME>; // block light (low nibble), sky light (high nibble)

    enum class State : uint8_t {
        Pending,    // created, waiting for terrain generation (server) or data (client)
        Generating, // worker is filling in blocks
        Loaded,     // block data is valid
    };

    // The 3x3x3 block of chunks around this one (the centre slot is unused).
    // Diagonal neighbours are needed for ambient occlusion at chunk edges.
    using Neighbours = std::array<std::shared_ptr<const Chunk>, 27>;
    static constexpr int NeighbourIndex(int dx, int dy, int dz) { return (dx + 1) + 3 * ((dy + 1) + 3 * (dz + 1)); }
    static int Index(int x, int y, int z) { return x + CHUNK_SIZE * (y + CHUNK_SIZE * z); }

    explicit Chunk(glm::ivec3 coords);
    virtual ~Chunk();

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    static int GetLiveCount() { return liveCount.load(); }

    glm::ivec3 GetCoords() const { return coords; }

    State GetState() const { return state.load(); }
    bool IsLoaded() const { return state.load() == State::Loaded; }

    // Removed chunks are skipped by the worker and never rendered again.
    void MarkRemoved() { removed = true; }
    bool IsRemoved() const { return removed.load(); }

    // Server: blocks changed since the chunk was last saved. Set by every edit;
    // TakeUnsaved clears it, so take it before copying the blocks out to save them
    // (an edit in between just marks the chunk again).
    void MarkUnsaved() { unsaved.store(true, std::memory_order_release); }
    bool IsUnsaved() const { return unsaved.load(std::memory_order_acquire); }
    bool TakeUnsaved() { return unsaved.exchange(false, std::memory_order_acq_rel); }

    // Block access, in chunk-local coordinates.
    Block GetBlock(int x, int y, int z) const;
    bool IsOpaque(int x, int y, int z) const;
    void SetBlock(int x, int y, int z, const Block& block);

    // Light, 0..kMaxLight per channel. Server worker thread only.
    uint8_t GetLight(LightChannel channel, int x, int y, int z) const {
        uint8_t v = light[Index(x, y, z)];
        return channel == LightChannel::Block ? (v & 0x0F) : (v >> 4);
    }
    void SetLight(LightChannel channel, int x, int y, int z, uint8_t value) {
        uint8_t& v = light[Index(x, y, z)];
        v = channel == LightChannel::Block ? static_cast<uint8_t>((v & 0xF0) | value)
                                           : static_cast<uint8_t>((v & 0x0F) | (value << 4));
    }

    // Sky light in the cell just above each column while the chunk above isn't loaded
    // (from the generator; see LightPropagator). Worker thread only.
    uint8_t GetSkyFromAbove(int x, int z) const { return skyFromAbove[x * CHUNK_SIZE + z]; }
    void SetSkyFromAbove(const std::array<uint8_t, CHUNK_SIZE * CHUNK_SIZE>& values) { skyFromAbove = values; }

    // Server worker: makes the current light visible to other threads and bumps the
    // light version, if it changed since the last call (or was never published).
    // Returns whether it did. Any thread: read the last published light / its version
    // (0 means never published).
    bool PublishLight();
    void CopyPublishedLight(Light& out) const;
    uint32_t GetLightVersion() const { return lightVersion.load(std::memory_order_acquire); }

    // Client: light received from the server. Returns which cells changed (empty if
    // none), so neighbours whose meshes sample them can be rebuilt.
    void SetLightData(const Light& values, std::vector<int>* changedCells = nullptr);

    // Copies all blocks out under a single lock.
    void CopyBlocks(Blocks& out) const;

    // Generation (server worker) or chunk data from the network (client). The blocks
    // are handed over here and the chunk becomes Loaded.
    bool TryBeginGenerate();
    void SetGenerated(const Blocks& generated);

private:
    friend class ChunkMesher;
    static std::atomic<int> liveCount;

    const glm::ivec3 coords;

    mutable std::mutex blockMutex;
    Blocks blocks{};
    Light light{}; // server: worker thread only; client: guarded by blockMutex
    Light publishedLight{}; // server: guarded by blockMutex
    std::array<uint8_t, CHUNK_SIZE * CHUNK_SIZE> skyFromAbove = MakeFullSky(); // worker thread only
    static std::array<uint8_t, CHUNK_SIZE * CHUNK_SIZE> MakeFullSky() { std::array<uint8_t, CHUNK_SIZE * CHUNK_SIZE> a; a.fill(kMaxLight); return a; }

    std::atomic<State> state{State::Pending};
    std::atomic_bool removed{false};
    std::atomic_bool unsaved{false};
    std::atomic<uint32_t> lightVersion{0};
};
