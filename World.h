#pragma once

#include <unordered_map>
#include <tuple>
#include <mutex>
#include <glm.hpp>
#include <atomic>

#include "Block.h"
#include "Chunk.h"
#include "TerrainGenerator.h"

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

    Block GetBlock(int x, int y, int z);
    bool GetBlockCulls(int x, int y, int z);
    void SetBlock(int x, int y, int z, Block block);
    void SetBlock(int x, int y, int z, BlockType type);
    void SetBlock(int x, int y, int z, EdgeData edges);

    void UpdateChunks(glm::vec3 position);
    void LoadChunks();

    void Render(Shader& shader, glm::mat4& viewMatrix, glm::mat4& projectionMatrix, float frameWidth, float frameHeight);

    TerrainGenerator* terrainGenerator;

private:
    std::unordered_map<std::tuple<int, int, int>, std::shared_ptr<Chunk>, hash_tuple> chunks;

    static const size_t LOAD_QUEUE_SIZE = 1024;
    std::mutex loadQueueLock;
    std::array<std::shared_ptr<Chunk>, LOAD_QUEUE_SIZE> loadQueue;
    std::atomic_int head_;
    std::atomic_int tail_;

    std::tuple<int, int, int> WorldToChunkCoordinates(glm::vec3 position);
    std::tuple<int, int, int> WorldToChunkCoordinates(int x, int y, int z);
    std::tuple<int, int, int> WorldToBlockCoordinates(int x, int y, int z);
    void UpdateAdjacentChunks(int x, int y, int z);
    void MarkAdjacentChunks(std::tuple<int, int, int> chunkCoords);
    void QueueChunkLoad(std::shared_ptr<Chunk> chunk);
};

