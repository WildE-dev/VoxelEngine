#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

#include "world/Block.h"

// A CHUNK_SIZE^3 block of voxels.
//
// Threading contract:
//  - Block data is guarded by an internal mutex and may be read/written from any thread.
//  - SetGenerated() and BuildMesh() run on the world worker thread.
//  - Light (block and sky) is only ever read or written on the worker thread (by the light
//    propagation and the mesher), so it needs no lock.
//  - UploadPendingMesh(), Draw() and ReleaseGpu() must run on the thread that owns the GL context.
//  - The destructor never touches GL, so the last reference may be dropped on any thread.
class Chunk
{
public:
    // Vertex positions are packed as (pos * 8) into 8 bits, so 31 is the maximum.
    static constexpr int CHUNK_SIZE = 16;
    static constexpr int VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

    enum class State : uint8_t {
        Pending,    // created, waiting for terrain generation
        Generating, // worker is filling in blocks
        Loaded,     // block data is valid
    };

    // The 3x3x3 block of chunks around this one (the centre slot is unused).
    // Diagonal neighbours are needed for ambient occlusion at chunk edges.
    using Neighbours = std::array<std::shared_ptr<const Chunk>, 27>;
    static constexpr int NeighbourIndex(int dx, int dy, int dz) { return (dx + 1) + 3 * ((dy + 1) + 3 * (dz + 1)); }

    explicit Chunk(glm::ivec3 coords);
    ~Chunk();

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    static int GetLiveCount() { return liveCount.load(); }

    glm::ivec3 GetCoords() const { return coords; }

    State GetState() const { return state.load(); }
    bool IsLoaded() const { return state.load() == State::Loaded; }

    // Removed chunks are skipped by the worker and never rendered again.
    void MarkRemoved() { removed = true; }
    bool IsRemoved() const { return removed.load(); }

    // Dirty flag used to de-duplicate mesh rebuild requests.
    // MarkDirty returns true if the chunk was clean (i.e. the caller should queue it).
    bool MarkDirty() { return !dirty.exchange(true); }
    void ClearDirty() { dirty = false; }

    // Block access, in chunk-local coordinates.
    Block GetBlock(int x, int y, int z) const;
    bool IsOpaque(int x, int y, int z) const;
    void SetBlock(int x, int y, int z, const Block& block);

    // Light, 0..kMaxLight per channel. Worker thread only.
    uint8_t GetLight(LightChannel channel, int x, int y, int z) const {
        uint8_t v = light[Index(x, y, z)];
        return channel == LightChannel::Block ? (v & 0x0F) : (v >> 4);
    }
    void SetLight(LightChannel channel, int x, int y, int z, uint8_t value) {
        uint8_t& v = light[Index(x, y, z)];
        v = channel == LightChannel::Block ? static_cast<uint8_t>((v & 0xF0) | value)
                                           : static_cast<uint8_t>((v & 0x0F) | (value << 4));
    }

    // Copies all blocks out under a single lock.
    void CopyBlocks(std::array<Block, VOLUME>& out) const;

    // Worker thread. Generation itself is TerrainGenerator's job; the result is
    // handed over here and the chunk becomes Loaded.
    bool TryBeginGenerate();
    void SetGenerated(const std::array<Block, VOLUME>& generated);
    void BuildMesh(const Neighbours& neighbours);

    // GL thread. Upload the latest mesh from the worker (if any), then draw it.
    void UploadPendingMesh();
    void Draw() const;
    void ReleaseGpu();
    bool HasGeometry() const { return vertexCount > 0; }

private:
    static std::atomic<int> liveCount;

    static int Index(int x, int y, int z) { return x + CHUNK_SIZE * (y + CHUNK_SIZE * z); }

    const glm::ivec3 coords;

    mutable std::mutex blockMutex;
    std::array<Block, VOLUME> blocks{};
    std::array<uint8_t, VOLUME> light{}; // block light (low nibble), sky light (high nibble); worker thread only

    // Mesh hand-off from the worker to the GL thread.
    std::mutex meshMutex;
    std::vector<uint32_t> pendingMesh;
    bool hasPendingMesh = false;

    // GL thread only.
    GLuint vao = 0, vbo = 0;
    GLsizei vertexCount = 0;

    std::atomic<State> state{State::Pending};
    std::atomic_bool dirty{false};
    std::atomic_bool removed{false};
};
