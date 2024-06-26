#pragma once

#include <unordered_map>
#include <tuple>

#include "Block.h"
#include "Chunk.h"

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
    World();
    bool GetChunk(Chunk*& chunk, int chunkX, int chunkY, int chunkZ);

    Block GetBlock(int x, int y, int z);
    bool GetBlockCulls(int x, int y, int z);
    void SetBlock(int x, int y, int z, unsigned char type);

    void Render(Shader& shader, glm::mat4& viewMatrix, glm::mat4& projectionMatrix, float frameWidth, float frameHeight);

private:
    std::unordered_map<std::tuple<int, int, int>, std::unique_ptr<Chunk>, hash_tuple> chunks;

    std::tuple<int, int, int> GetChunkCoordinates(int x, int y, int z);
    std::tuple<int, int, int> GetBlockCoordinates(int x, int y, int z);
    void UpdateAdjacentChunks(int x, int y, int z);
};

