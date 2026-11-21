#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "client/ClientChunk.h"
#include "world/BlockAccess.h"
#include "world/Coords.h"

class Shader;

// The client's copy of the world: chunks received from the server, meshed on a
// background worker and drawn. It never generates terrain, propagates light or
// simulates water; all of that arrives from the server.
//
// Threading model:
//  - The main (GL) thread applies network data and is the only one that inserts into
//    or erases from the chunk map, so it can read the map without locking. Those
//    writes take `chunksMutex` exclusively; the worker takes it shared to look up
//    neighbours.
//  - Chunks are shared_ptrs so a chunk being meshed stays alive even if it's unloaded
//    meanwhile. Unloaded chunks are flagged as removed and have their GL objects
//    released on the main thread before being dropped.
//  - Finished meshes are handed to the chunk, and the chunk is queued for the main
//    thread to upload at the start of the next frame.
//  - GetBlock is safe from any thread.
//
// Drawing: only chunks that have a mesh are kept in a draw list (most loaded chunks are
// solid ground or empty air). Each frame the list is culled against the view frustum
// and drawn nearest first, so nearer terrain hides farther terrain before it's shaded.
class ClientWorld : public BlockAccess
{
public:
    ClientWorld();
    // Must be destroyed while the GL context is still current (if any chunk was drawn).
    ~ClientWorld() override;

    ClientWorld(const ClientWorld&) = delete;
    ClientWorld& operator=(const ClientWorld&) = delete;

    // View distance as told by the server (Welcome).
    void SetViewDistance(int horizontal, int vertical);
    int GetViewDistance() const { return renderDistance; } // chunks, horizontally

    // Main thread: track the chunks expected around the camera (see Update in the .cpp).
    void Update(const glm::vec3& cameraPosition);
    void Render(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time);
    // Water, after all opaque geometry, front to back. Draws the chunks Render found
    // visible, so call it after Render with the same camera.
    void RenderWater(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time);

    // Whether any chunk the last Render found visible has water.
    bool HasVisibleWater() const { return visibleWater; }

    // Chunks with a mesh, and how many of them the last Render drew.
    size_t GetMeshedChunkCount() const { return drawList.size(); }
    size_t GetDrawnChunkCount() const { return visible.size(); }

    // Network data, main thread.
    void ApplyChunk(const glm::ivec3& coords, const Chunk::Blocks& blocks, const Chunk::Light& light);
    void ApplyLight(const glm::ivec3& coords, const Chunk::Light& light);
    void UnloadChunk(const glm::ivec3& coords);
    // A block change from the server, or the local prediction of our own edit.
    void SetBlock(int x, int y, int z, const Block& block);

    bool GetBlock(int x, int y, int z, Block& block) const override;
    // The light in a cell (0..15 each), or false if its chunk isn't here.
    bool GetLight(int x, int y, int z, int& blockLight, int& skyLight) const;

    void RebuildAllChunks();

    // Blocks drawn as models (Block::IsModel: water wheels) in the loaded chunks.
    // Main thread.
    std::vector<glm::ivec3> GetModelBlocks() const;

    size_t GetChunkCount() const { return chunks.size(); }
    size_t GetLoadedChunkCount() const { return loadedCount; }

private:
    using ChunkPtr = std::shared_ptr<ClientChunk>;

    // Caller must be the main thread or hold `chunksMutex`.
    ChunkPtr FindChunk(const glm::ivec3& chunkCoords) const;
    ChunkPtr FindChunkShared(const glm::ivec3& chunkCoords) const;

    void RefreshPlaceholders(const glm::ivec3& center);
    void RequestRebuild(const ChunkPtr& chunk);
    void RequestNeighbourRebuilds(const glm::ivec3& chunkCoords);
    void RemoveChunk(const glm::ivec3& coords);

    void WorkerLoop();
    // Returns false (and builds nothing) while a neighbour is still waiting for data.
    bool BuildChunkMesh(ClientChunk& chunk);

    std::unordered_map<glm::ivec3, ChunkPtr, Coords::IVec3Hash> chunks;
    mutable std::shared_mutex chunksMutex;
    size_t loadedCount = 0; // main thread

    int renderDistance = 0, verticalRenderDistance = 0;
    std::vector<glm::ivec3> loadOffsets;
    glm::ivec3 lastCenter{0};
    bool hasCenter = false;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<ChunkPtr> rebuildQueue;
    bool stopping = false;
    std::thread worker;

    std::vector<int> changedCells; // ApplyLight scratch
    // Model blocks by chunk (main thread: kept up to date as chunks and edits arrive).
    std::unordered_map<glm::ivec3, std::vector<glm::ivec3>, Coords::IVec3Hash> modelBlocks;

    // Chunks the worker has new meshes for, waiting for the main thread to upload them.
    std::mutex uploadMutex;
    std::vector<ChunkPtr> uploadQueue;
    std::vector<ChunkPtr> uploading; // main thread scratch

    // Main thread. Uploads queued meshes and rebuilds the draw list if it changed.
    void PrepareDraw();
    struct DrawItem {
        ClientChunk* chunk;
        glm::vec3 origin;
        float distance; // squared, camera to chunk centre
    };
    std::vector<DrawItem> drawList; // every chunk with opaque geometry or water
    std::vector<DrawItem> visible;  // those in the frustum, nearest first
    bool drawListDirty = false;
    bool visibleWater = false;
    // Render's flood through the chunks (scratch).
    struct FloodStep {
        glm::ivec3 coords;
        int8_t entry;       // the face it was entered by (-1: the camera's chunk)
        uint8_t directions; // the directions stepped so far
    };
    std::vector<uint8_t> floodSeen;
    std::vector<FloodStep> floodQueue;
};
