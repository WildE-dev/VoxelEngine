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
#include "world/BlockAccess.h"
#include "world/Chunk.h"
#include "world/Coords.h"
#include "world/Lighting.h"
#include "world/TerrainGenerator.h"
#include "server/Water.h"

class WorldStorage;

// The authoritative world: owns all chunks around the connected players, a background
// worker that generates terrain and propagates light, and the water simulation. No GL.
//
// Threading model:
//  - The server thread is the only one that inserts into or erases from the chunk map,
//    so it can read the map without locking. Those writes take `chunksMutex`
//    exclusively; the worker and water threads take it shared to look up a chunk.
//  - Chunks are shared_ptrs so a chunk the worker is processing stays alive even if
//    the server thread unloads it meanwhile.
//  - Light lives entirely on the worker: edits only queue the positions of changed
//    blocks, and the worker propagates light and publishes it per chunk (see
//    Chunk::PublishLight), logging which chunks' light changed.
//  - Flowing water runs on its own thread. GetBlock and SetBlock are safe from the
//    server and water threads: lookups take `chunksMutex` shared, and every edit holds
//    `editMutex`, which the water thread also holds around each slice of cell updates,
//    so its read-modify-writes can't interleave with player edits.
//    Lock order: editMutex, then chunksMutex, then a chunk's block mutex, then
//    queueMutex, then logMutex.
//  - Every edit that changes what a client sees is logged (TakeChanges), so the server
//    can stream it to the clients that have the chunk.
//  - With a WorldStorage, edited chunks are saved when they unload and by SaveAll, and
//    chunks that were saved load from it instead of being generated.
class ServerWorld : public BlockAccess
{
public:
    static constexpr int RENDER_DISTANCE = 12;          // horizontal, in chunks
    static constexpr int VERTICAL_RENDER_DISTANCE = 12; // vertical, in chunks
    // Flowing water is only simulated this many chunks (per axis) around a player;
    // further out it pauses until someone comes back (see WaterSimulation).
    static constexpr int DEFAULT_WATER_SIMULATION_DISTANCE = 6;

    // `storage` may be null (nothing is saved); otherwise it must outlive the world.
    // renderDistance: chunks loaded around each player (horizontally; up to
    // RENDER_DISTANCE). Vertically the same, but at least 4 and at most the vertical limit.
    explicit ServerWorld(const TerrainGenerator& terrain, WorldStorage* storage = nullptr, int renderDistance = RENDER_DISTANCE);
    int GetRenderDistance() const { return renderDistance; }
    int GetVerticalRenderDistance() const { return verticalRenderDistance; }
    ~ServerWorld() override;

    ServerWorld(const ServerWorld&) = delete;
    ServerWorld& operator=(const ServerWorld&) = delete;

    // Server thread: keep chunks loaded around these chunk coordinates (one per player)
    // and unload the rest.
    void Update(const std::vector<glm::ivec3>& centers);

    // World coordinates; server or water thread.
    bool GetBlock(int x, int y, int z, Block& block) const override;
    void SetBlock(int x, int y, int z, const Block& block);
    void SetBlock(int x, int y, int z, BlockType type) { SetBlock(x, y, z, Block(type)); }

    // Server thread: the chunk at these coordinates (any state), or null.
    std::shared_ptr<const Chunk> FindChunk(const glm::ivec3& chunkCoords) const;

    // Block positions whose visible state changed since the last call (water that only
    // changed volume within the same drawn surface height is left out), and chunks whose
    // published light changed. Any thread.
    void TakeChanges(std::vector<glm::ivec3>& blocks, std::vector<glm::ivec3>& lightChunks);

    // Starts simulating flowing water on its own thread. Without it, water only moves
    // when WaterSimulation::Tick is called (tests step it deterministically).
    void StartWaterThread() { water.Start(*this); }
    // Server thread, before the first Update. Clamped to 1..render distance - 1:
    // water needs loaded chunks around it to flow into.
    void SetWaterSimulationDistance(int chunks);
    int GetWaterSimulationDistance() const { return waterDistance; }
    WaterSimulation& GetWater() { return water; }
    // Held for every block edit; the water thread holds it around each update slice.
    std::recursive_mutex& GetEditMutex() { return editMutex; }
    // Edits waiting for (or holding) the edit mutex. The water thread lets them go
    // first between slices: the mutex itself isn't fair, so without this an edit could
    // lose the race for it several slices in a row.
    bool HasPendingEdits() const { return pendingEdits.load(std::memory_order_acquire) > 0; }

    size_t GetLoadedChunkCount() const { return chunks.size(); }

    // Server thread: hands every loaded chunk edited since it was last saved to the
    // storage (call WorldStorage::Flush afterwards to write it out). Takes the edit
    // mutex while copying, so the snapshot never catches flowing water halfway between
    // two chunks. Returns the number of chunks saved.
    size_t SaveAll();

private:
    using ChunkPtr = std::shared_ptr<Chunk>;

    // Caller must be the server thread or hold `chunksMutex`.
    ChunkPtr FindChunkUnlocked(const glm::ivec3& chunkCoords) const;
    // Any thread: takes `chunksMutex` shared for the lookup.
    ChunkPtr FindChunkShared(const glm::ivec3& chunkCoords) const;

    void WorkerLoop();
    // Worker thread: heights for a chunk column, cached since every chunk stacked in
    // the column needs the same (expensive) noise samples.
    const TerrainGenerator::ColumnHeights& GetColumnHeights(int chunkX, int chunkZ);
    // Worker thread: runs light propagation and publishes the light of affected chunks.
    void PropagateLight(const std::vector<ChunkPtr>& loadedChunks, const std::vector<glm::ivec3>& changedBlocks);

    const TerrainGenerator& terrain;
    WorldStorage* const storage;

    std::unordered_map<glm::ivec3, ChunkPtr, Coords::IVec3Hash> chunks;
    mutable std::shared_mutex chunksMutex;
    std::recursive_mutex editMutex; // recursive: SetBlock may uproot a plant above
    std::atomic<int> pendingEdits{0};

    // Offsets within render distance, nearest first, so close chunks load first.
    std::vector<glm::ivec3> loadOffsets;
    std::vector<glm::ivec3> lastCenters;
    int waterDistance = DEFAULT_WATER_SIMULATION_DISTANCE;
    int renderDistance = RENDER_DISTANCE, verticalRenderDistance = VERTICAL_RENDER_DISTANCE;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<ChunkPtr> generateQueue;
    std::vector<glm::ivec3> lightQueue; // edited block positions
    bool stopping = false;

    std::mutex logMutex;
    std::vector<glm::ivec3> changedBlocks;
    std::vector<glm::ivec3> changedLight;

    struct IVec2Hash {
        size_t operator()(const glm::ivec2& v) const noexcept { return Coords::IVec3Hash{}(glm::ivec3(v, 0)); }
    };
    std::unordered_map<glm::ivec2, std::unique_ptr<TerrainGenerator::ColumnHeights>, IVec2Hash> columnHeights; // worker thread only
    LightPropagator light; // worker thread only
    WaterSimulation water;
    std::thread worker;
};
