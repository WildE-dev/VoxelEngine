#pragma once

#include <atomic>
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
#include "world/Water.h"

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
//  - Flowing water runs on its own thread (StartWaterThread). GetBlock and SetBlock
//    are safe from the main and water threads: lookups take `chunksMutex` shared, and
//    every edit holds `editMutex`, which the water thread also holds around each slice
//    of cell updates, so its read-modify-writes can't interleave with player edits.
//    Lock order: editMutex, then chunksMutex, then a chunk's block mutex, then
//    queueMutex.
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
    // Translucent water, after all opaque geometry: chunks drawn back to front.
    void RenderWater(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time, const glm::vec3& cameraPosition);

    // World coordinates; main or water thread. GetBlock returns false if the chunk
    // isn't loaded.
    bool GetBlock(int x, int y, int z, Block& block) const;
    void SetBlock(int x, int y, int z, const Block& block);
    void SetBlock(int x, int y, int z, BlockType type) { SetBlock(x, y, z, Block(type)); }

    // Main or water thread: whether a point is below the water surface in its cell.
    bool IsUnderwater(const glm::vec3& point) const;

    void RebuildAllChunks();

    // Starts simulating flowing water on its own thread. Without it, water only moves
    // when WaterSimulation::Tick is called (tests step it deterministically).
    void StartWaterThread() { water.Start(*this); }
    WaterSimulation& GetWater() { return water; }
    // Held for every block edit; the water thread holds it around each update slice.
    std::recursive_mutex& GetEditMutex() { return editMutex; }
    // Edits waiting for (or holding) the edit mutex. The water thread lets them go
    // first between slices: the mutex itself isn't fair, so without this an edit could
    // lose the race for it several slices in a row.
    bool HasPendingEdits() const { return pendingEdits.load(std::memory_order_acquire) > 0; }

    size_t GetLoadedChunkCount() const { return chunks.size(); }

    static glm::ivec3 WorldToChunkCoordinates(const glm::vec3& position);
    static glm::ivec3 WorldToChunkCoordinates(int x, int y, int z);
    static glm::ivec3 WorldToLocalCoordinates(int x, int y, int z);

private:
    using ChunkPtr = std::shared_ptr<Chunk>;

    // Caller must be the main thread or hold `chunksMutex`.
    ChunkPtr FindChunk(const glm::ivec3& chunkCoords) const;
    // Any thread: takes `chunksMutex` shared for the lookup.
    ChunkPtr FindChunkShared(const glm::ivec3& chunkCoords) const;

    void RefreshChunksAround(const glm::ivec3& center);
    void RequestRebuild(const ChunkPtr& chunk);
    void RequestNeighbourRebuilds(const glm::ivec3& chunkCoords);

    void WorkerLoop();
    // Worker thread: heights for a chunk column, cached since every chunk stacked in
    // the column needs the same (expensive) noise samples.
    const TerrainGenerator::ColumnHeights& GetColumnHeights(int chunkX, int chunkZ);
    // Returns false (and builds nothing) while a neighbour is still waiting to generate.
    bool BuildChunkMesh(Chunk& chunk);
    // Worker thread: runs light propagation and queues rebuilds for affected chunks.
    void PropagateLight(const std::vector<ChunkPtr>& loadedChunks, const std::vector<glm::ivec3>& changedBlocks);

    const TerrainGenerator& terrain;

    std::unordered_map<glm::ivec3, ChunkPtr, Coords::IVec3Hash> chunks;
    mutable std::shared_mutex chunksMutex;
    std::recursive_mutex editMutex; // recursive: SetBlock may uproot a plant above
    std::atomic<int> pendingEdits{0};

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
    std::unordered_map<glm::ivec2, std::unique_ptr<TerrainGenerator::ColumnHeights>, IVec2Hash> columnHeights; // worker thread only
    LightPropagator light; // worker thread only
    WaterSimulation water; // main thread only
    std::thread worker;
};
