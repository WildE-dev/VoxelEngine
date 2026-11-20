#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

#include "world/Block.h"

class TerrainGenerator;

// A CHUNK_SIZE^3 block of voxels.
//
// Threading contract:
//  - Block data is guarded by an internal mutex and may be read/written from any thread.
//  - Generate() and BuildMesh() run on the world worker thread.
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

    using Neighbours = std::array<std::shared_ptr<const Chunk>, kFaceCount>;

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

    // Worker thread.
    bool TryBeginGenerate();
    void Generate(const TerrainGenerator& terrain);
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
