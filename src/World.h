#pragma once

#include <unordered_map>
#include <tuple>
#include <mutex>
#include <glm/glm.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include <queue>

#include "Block.h"
#include "Chunk.h"
#include "TerrainGenerator.h"
#include "Camera.h"

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
    static const int RENDER_DISTANCE = 12;

    World(TerrainGenerator* terrainGenerator);
    World(const World& other);
    bool GetChunk(Chunk*& chunk, int chunkX, int chunkY, int chunkZ);
    std::unique_ptr<Chunk> GetChunk(int chunkX, int chunkY, int chunkZ);

    bool GetBlock(int x, int y, int z, Block& block);
    bool GetBlockCulls(int x, int y, int z);
    void SetBlock(int x, int y, int z, Block block);
    void SetBlock(int x, int y, int z, BlockType type);
    void SetBlock(int x, int y, int z, EdgeData edges);

    void WorldThread();

    void Update(Camera* camera);

    void RebuildAllChunks();

    void Render(Shader& shader, glm::mat4& viewMatrix, glm::mat4& projectionMatrix, float frameWidth, float frameHeight, float time);

    void Stop();

    TerrainGenerator* terrainGenerator;

private:
    std::unordered_map<std::tuple<int, int, int>, std::unique_ptr<Chunk>, hash_tuple> chunks;

    static const int ASYNC_NUM_CHUNKS_PER_FRAME = 12;

    //AsyncCircularQueue<std::shared_ptr<Chunk>> chunkQueue;

    glm::vec3 m_cameraPosition, m_cameraView;

    std::vector<std::unique_ptr<Chunk>> m_vpChunkSetupList, m_vpChunkUpdateFlagsList;
    std::vector<Chunk&> m_vpChunkRenderList;

    glm::ivec3 WorldToChunkCoordinates(glm::vec3 position);
    glm::ivec3 WorldToChunkCoordinates(int x, int y, int z);
    glm::ivec3 WorldToBlockCoordinates(int x, int y, int z);
    void UpdateAdjacentChunks(int x, int y, int z);
    void UpdateAdjacentChunks(Chunk& chunk);

    bool m_forceVisibilityUpdate;

    void UpdateAsyncChunker();
    void UpdateLoadList();
    void UpdateSetupList();
    void UpdateRebuildList();
    void UpdateFlagsList();
    void UpdateUnloadList();
    void UpdateRenderList();

    bool running;

    std::queue<glm::ivec3> m_chunkLoadQueue;
    std::queue<std::unique_ptr<Chunk>> m_chunkUnloadQueue;
    std::queue<std::unique_ptr<Chunk>> m_chunkRebuildQueue;

    std::queue<std::unique_ptr<Chunk>> m_chunkReadyQueue;

    std::mutex m_chunkLoadQueueMutex;
    std::mutex m_chunkUnloadQueueMutex;
    std::mutex m_chunkRebuildQueueMutex;

    std::mutex m_chunkReadyQueueMutex;
};

