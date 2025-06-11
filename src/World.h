#pragma once

#include <unordered_map>
#include <tuple>
#include <mutex>
#include <glm/glm.hpp>
#include <atomic>
#include <thread>
#include <chrono>

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
    Chunk* GetChunk(int chunkX, int chunkY, int chunkZ);

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

    std::vector<Chunk*> m_vpChunkLoadList, m_vpChunkSetupList, m_vpChunkRebuildList, m_vpChunkUpdateFlagsList, m_vpChunkUnloadList, m_vpChunkVisibilityList, m_vpChunkRenderList;

    glm::ivec3 WorldToChunkCoordinates(glm::vec3 position);
    glm::ivec3 WorldToChunkCoordinates(int x, int y, int z);
    glm::ivec3 WorldToBlockCoordinates(int x, int y, int z);
    void UpdateAdjacentChunks(int x, int y, int z);
    void UpdateAdjacentChunks(Chunk* chunk);

    bool m_forceVisibilityUpdate;

    void UpdateAsyncChunker();
    void UpdateLoadList();
    void UpdateSetupList();
    void UpdateRebuildList();
    void UpdateFlagsList();
    void UpdateUnloadList();
    void UpdateVisibilityList();
    void UpdateRenderList();

    std::mutex cameraMutex;
	std::mutex chunksMutex;
    bool running;
};

