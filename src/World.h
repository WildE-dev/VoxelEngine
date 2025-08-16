#pragma once

#include <unordered_map>
#include <tuple>
#include <mutex>
#include <glm/glm.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include <set>
#include <future>

#include "Block.h"
#include "Chunk.h"
#include "TerrainGenerator.h"
#include "Camera.h"
#include "ThreadPool.h"

inline int cantor(int a, int b) {
    return (a + b + 1) * (a + b) / 2 + b;
}

struct hash_tuple {
    size_t operator()(const std::tuple<int, int, int>& x) const {
        return cantor(std::get<0>(x), cantor(std::get<1>(x), std::get<2>(x)));
    }
};

class Shader;

class World
{
public:
    static const int RENDER_DISTANCE = 4;

    World(TerrainGenerator* terrainGenerator);
    World(const World& other);
    Chunk* GetChunk(int chunkX, int chunkY, int chunkZ);

    bool GetBlock(int x, int y, int z, Block& block);
    bool GetBlockCulls(int x, int y, int z);

    void SetBlock(int x, int y, int z, Block block);
    void SetBlocksBatch(const std::vector<std::tuple<int, int, int, Block>>& blocks);
    void SetBlock(int x, int y, int z, BlockType type);
    void SetBlock(int x, int y, int z, EdgeData edges);

    void WorldThread();

    void Update(Camera* camera);

    void RebuildAllChunks();

    void Render(Shader& shader, glm::mat4& viewMatrix, glm::mat4& projectionMatrix, float frameWidth, float frameHeight, float time);

    void Stop();

    void QueueMeshGeneration(Chunk* chunk);
    void ProcessMeshQueue();

    TerrainGenerator* terrainGenerator;

    void DebugFixChunk(glm::vec3 position);
    void DebugFixChunk(int chunkX, int chunkY, int chunkZ);

private:
    std::unordered_map<std::tuple<int, int, int>, std::unique_ptr<Chunk>, hash_tuple> chunks;

    static const int ASYNC_NUM_CHUNKS_PER_FRAME = 25;

    static const int MESH_GENERATION_THREADS = 4; // Dedicated mesh generation threads
    ThreadPool meshThreadPool;

    // Batch processing
    std::vector<Chunk*> meshGenerationQueue;
    std::mutex meshQueueMutex;

    glm::vec3 m_cameraPosition, m_cameraView;

    std::vector<Chunk*> m_vpChunkLoadList, m_vpChunkSetupList, m_vpChunkRebuildList, m_vpChunkUpdateFlagsList, m_vpChunkVisibilityList, m_vpChunkRenderList;
    std::vector<std::unique_ptr<Chunk>> m_vpChunkUnloadedList;

    glm::ivec3 WorldToChunkCoordinates(glm::vec3 position);
    glm::ivec3 WorldToChunkCoordinates(int x, int y, int z);
    glm::ivec3 WorldToBlockCoordinates(int x, int y, int z);
    void UpdateAdjacentChunks(int x, int y, int z);
    void UpdateAdjacentChunks(Chunk* chunk);

    // Main thread
    void UpdateAsyncChunker();
    void UpdateRenderList();

    // Chunk thread
    void UpdateLoadList();
    void UpdateSetupList();
    void UpdateRebuildList();
    void UpdateFlagsList();
    void UpdateVisibilityList();

    std::mutex chunksMutex;
    std::mutex loadListMutex;
    std::mutex setupListMutex;
    std::mutex rebuildListMutex;
    std::mutex flagsListMutex;
    std::mutex visibilityListMutex;

    std::atomic<bool> running{ true };
    std::atomic<bool> m_forceVisibilityUpdate{ false };

    struct BlockModification {
        int x, y, z;
        Block block;
    };
    std::vector<BlockModification> pendingModifications;
    std::mutex modificationsMutex;

    void ProcessPendingModifications();

    std::condition_variable workAvailable;
    std::mutex workMutex;
};

