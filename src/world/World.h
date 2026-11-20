#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "world/Block.h"
#include "world/Chunk.h"
#include "world/Coords.h"
#include "world/Lighting.h"
#include "world/TerrainGenerator.h"

class Shader;

// Owns all chunks and a background worker that generates terrain and builds meshes.
//
// Threading model:
//  - The main (GL) thread is the only one that inserts into or erases from the chunk
//    map, so it can read the map without locking. Those writes take `chunksMutex`
//    exclusively; the worker takes it shared whenever it looks up a chunk.
//  - Chunks are shared_ptrs so a chunk the worker is processing stays alive even if
//    the main thread unloads it meanwhile. Unloaded chunks are flagged as removed and
//    have their GL objects released on the main thread before being dropped.
//  - Finished meshes are handed to the chunk and uploaded lazily on the main thread.
//  - Block light lives entirely on the worker: the main thread only queues the
//    positions of edited blocks, and the worker propagates light before meshing.
class World
{
public:
    static constexpr int RENDER_DISTANCE = 12;          // horizontal, in chunks
    static constexpr int VERTICAL_RENDER_DISTANCE = 12; // vertical, in chunks

    explicit World(const TerrainGenerator& terrain);
    // Must be destroyed while the GL context is still current.
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Main thread: load/unload chunks around the camera.
    void Update(const glm::vec3& cameraPosition);
    void Render(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time);

    // Main thread, world coordinates. GetBlock returns false if the chunk isn't loaded.
    bool GetBlock(int x, int y, int z, Block& block) const;
    void SetBlock(int x, int y, int z, const Block& block);
    void SetBlock(int x, int y, int z, BlockType type) { SetBlock(x, y, z, Block(type)); }

    void RebuildAllChunks();

    size_t GetLoadedChunkCount() const { return chunks.size(); }

    static glm::ivec3 WorldToChunkCoordinates(const glm::vec3& position);
    static glm::ivec3 WorldToChunkCoordinates(int x, int y, int z);
    static glm::ivec3 WorldToLocalCoordinates(int x, int y, int z);

private:
    using ChunkPtr = std::shared_ptr<Chunk>;

    // Caller must be the main thread or hold `chunksMutex`.
    ChunkPtr FindChunk(const glm::ivec3& chunkCoords) const;

    void RefreshChunksAround(const glm::ivec3& center);
    void RequestRebuild(const ChunkPtr& chunk);
    void RequestNeighbourRebuilds(const glm::ivec3& chunkCoords);

    void WorkerLoop();
    // Worker thread: heights for a chunk column, cached since every chunk stacked in
    // the column needs the same (expensive) noise samples.
    const Chunk::ColumnHeights& GetColumnHeights(int chunkX, int chunkZ);
    // Returns false (and builds nothing) while a neighbour is still waiting to generate.
    bool BuildChunkMesh(Chunk& chunk);
    // Worker thread: runs light propagation and queues rebuilds for affected chunks.
    void PropagateLight(const std::vector<ChunkPtr>& loadedChunks, const std::vector<glm::ivec3>& changedBlocks);

    const TerrainGenerator& terrain;

    std::unordered_map<glm::ivec3, ChunkPtr, Coords::IVec3Hash> chunks;
    mutable std::shared_mutex chunksMutex;

    // Offsets within render distance, nearest first, so close chunks load first.
    std::vector<glm::ivec3> loadOffsets;
    glm::ivec3 lastCenter{0};
    bool hasCenter = false;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<ChunkPtr> generateQueue;
    std::deque<ChunkPtr> rebuildQueue;
    std::vector<glm::ivec3> lightQueue; // edited block positions
    bool stopping = false;

    struct IVec2Hash {
        size_t operator()(const glm::ivec2& v) const noexcept { return Coords::IVec3Hash{}(glm::ivec3(v, 0)); }
    };
    std::unordered_map<glm::ivec2, std::unique_ptr<Chunk::ColumnHeights>, IVec2Hash> columnHeights; // worker thread only
    LightPropagator light; // worker thread only
    std::thread worker;
};
