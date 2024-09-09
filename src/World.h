#pragma once

#include <unordered_map>
#include <tuple>
#include <mutex>
#include <glm/glm.hpp>
#include <atomic>

#include "Block.h"
#include "Chunk.h"
#include "TerrainGenerator.h"
#include "AsyncCircularQueue.h"

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
    static const int UNLOAD_DISTANCE = 6;

    World(TerrainGenerator* terrainGenerator);
    World(const World& other);
    bool GetChunk(Chunk*& chunk, int chunkX, int chunkY, int chunkZ);
    std::shared_ptr<Chunk> GetChunk(int chunkX, int chunkY, int chunkZ);

    Block GetBlock(int x, int y, int z);
    bool GetBlockCulls(int x, int y, int z);
    void SetBlock(int x, int y, int z, Block block);
    void SetBlock(int x, int y, int z, BlockType type);
    void SetBlock(int x, int y, int z, EdgeData edges);

    void Update(glm::vec3 cameraPosition, glm::vec3 cameraView);
    void LoadChunks();

    void Render(Shader& shader, glm::mat4& viewMatrix, glm::mat4& projectionMatrix, float frameWidth, float frameHeight);

    TerrainGenerator* terrainGenerator;

private:
    std::unordered_map<std::tuple<int, int, int>, std::shared_ptr<Chunk>, hash_tuple> chunks;

    static const int ASYNC_NUM_CHUNKS_PER_FRAME = 8;

    //AsyncCircularQueue<std::shared_ptr<Chunk>> chunkQueue;

    glm::vec3 m_cameraPosition, m_cameraView;

    std::vector<std::shared_ptr<Chunk>> m_vpChunkLoadList, m_vpChunkSetupList, m_vpChunkRebuildList, m_vpChunkUpdateFlagsList, m_vpChunkUnloadList, m_vpChunkVisibilityList, m_vpChunkRenderList;

    glm::ivec3 WorldToChunkCoordinates(glm::vec3 position);
    glm::ivec3 WorldToChunkCoordinates(int x, int y, int z);
    glm::ivec3 WorldToBlockCoordinates(int x, int y, int z);
    void UpdateAdjacentChunks(int x, int y, int z);
    void MarkAdjacentChunks(glm::ivec3 chunkCoords);
    void QueueChunkLoad(std::shared_ptr<Chunk> chunk);

    void UpdateAsyncChunker(glm::vec3 position);
    void UpdateLoadList();
    void UpdateSetupList();
    void UpdateRebuildList();
    void UpdateFlagsList();
    void UpdateUnloadList();
    void UpdateVisibilityList(glm::vec3 cameraPosition);
    void UpdateRenderList();
};

