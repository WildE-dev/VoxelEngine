#include <iostream>
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>

#include "World.h"
#include "Shader.h"

inline int rem(int a, int b) {
    return ((a % b) + b) % b;
}

World::World() {
    //chunks.reserve(128);
    for (int x = -5; x < 5; ++x) {
        for (int y = 0; y < 1; ++y) {
            for (int z = -5; z < 5; ++z) {
                auto key = std::make_tuple(x, y, z);
                chunks[key] = std::unique_ptr<Chunk>(new Chunk(this, x, y, z));
            }
        }
    }

    //auto key = std::make_tuple(0, 0, 0);
    //chunks[key] = std::unique_ptr<Chunk>(new Chunk(this, 0, 0, 0));

    for (auto& pair : chunks) {
        int chunkX = std::get<0>(pair.first);
        int chunkY = std::get<1>(pair.first);
        int chunkZ = std::get<2>(pair.first);

        (*pair.second).GenerateMesh(*this);
    }
}

std::tuple<int, int, int> World::GetChunkCoordinates(int x, int y, int z) {
    return std::make_tuple(std::floor((float)x / Chunk::CHUNK_SIZE),
        std::floor((float)y / Chunk::CHUNK_SIZE),
        std::floor((float)z / Chunk::CHUNK_SIZE));
}

std::tuple<int, int, int> World::GetBlockCoordinates(int x, int y, int z) {
    return std::make_tuple(rem(x, Chunk::CHUNK_SIZE), rem(y, Chunk::CHUNK_SIZE), rem(z, Chunk::CHUNK_SIZE));
}

bool World::GetChunk(Chunk*& chunk, int chunkX, int chunkY, int chunkZ)
{
    auto key = std::make_tuple(chunkX, chunkY, chunkZ);
    if (chunks.find(key) == chunks.end()) {
        return false;
    }

    chunk = chunks[key].get();
    return true;
}

Block World::GetBlock(int x, int y, int z) {
    auto chunkCoords = GetChunkCoordinates(x, y, z);
    auto blockCoords = GetBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
        return chunk->GetBlock(std::get<0>(blockCoords), std::get<1>(blockCoords), std::get<2>(blockCoords));
    }
    return Block();
}

bool World::GetBlockCulls(int x, int y, int z) {
    auto chunkCoords = GetChunkCoordinates(x, y, z);
    auto blockCoords = GetBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
        return chunk->GetBlock(std::get<0>(blockCoords), std::get<1>(blockCoords), std::get<2>(blockCoords)).type != BlockType::AIR;
    }
    return false;
}

void World::SetBlock(int x, int y, int z, BlockType type) {
    auto chunkCoords = GetChunkCoordinates(x, y, z);
    auto blockCoords = GetBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
        chunk->SetBlock(std::get<0>(blockCoords), std::get<1>(blockCoords), std::get<2>(blockCoords), type);
        UpdateAdjacentChunks(x, y, z);
    }
}

void World::UpdateAdjacentChunks(int x, int y, int z) {
    Chunk* chunk = nullptr;
    std::tuple<int, int, int> chunkCoords;
    std::tuple<int, int, int> blockCoords = GetBlockCoordinates(x, y, z);

    // Check -X boundary
    if (std::get<0>(blockCoords) == 0) {
        chunkCoords = GetChunkCoordinates(x - 1, y, z);
        if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
            chunk->GenerateMesh(*this);
        }
    }
    // Check +X boundary
    if (std::get<0>(blockCoords) == Chunk::CHUNK_SIZE - 1) {
        chunkCoords = GetChunkCoordinates(x + 1, y, z);
        if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
            chunk->GenerateMesh(*this);
        }
    }

    // Check -Y boundary
    if (std::get<1>(blockCoords) == 0) {
        chunkCoords = GetChunkCoordinates(x, y - 1, z);
        if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
            chunk->GenerateMesh(*this);
        }
    }
    // Check +Y boundary
    if (std::get<1>(blockCoords) == Chunk::CHUNK_SIZE - 1) {
        chunkCoords = GetChunkCoordinates(x, y + 1, z);
        if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
            chunk->GenerateMesh(*this);
        }
    }

    // Check -Z boundary
    if (std::get<2>(blockCoords) == 0) {
        chunkCoords = GetChunkCoordinates(x, y, z - 1);
        if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
            chunk->GenerateMesh(*this);
        }
    }
    // Check +Z boundary
    if (std::get<2>(blockCoords) == Chunk::CHUNK_SIZE - 1) {
        chunkCoords = GetChunkCoordinates(x, y, z + 1);
        if (GetChunk(chunk, std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords))) {
            chunk->GenerateMesh(*this);
        }
    }
}


void World::Render(Shader& shader, glm::mat4& viewMatrix, glm::mat4& projectionMatrix, float frameWidth, float frameHeight) {
    shader.Use();

    glm::mat4 model = glm::mat4(1.0f);

    shader.SetUniform("model", model);
    shader.SetUniform("projection", projectionMatrix);

    for (auto& pair : chunks) {
        int chunkX = std::get<0>(pair.first);
        int chunkY = std::get<1>(pair.first);
        int chunkZ = std::get<2>(pair.first);

        glm::mat4 newViewMatrix = glm::translate(viewMatrix, glm::vec3(chunkX * Chunk::CHUNK_SIZE, chunkY * Chunk::CHUNK_SIZE, chunkZ * Chunk::CHUNK_SIZE));
        shader.SetUniform("view", newViewMatrix);

        (*pair.second).Render(shader);
    }
}