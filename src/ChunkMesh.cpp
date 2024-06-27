#include <glm.hpp>
#include <ext/matrix_transform.hpp>
#include <iostream>

#include "ChunkMesh.h"
#include "Block.h"
#include "Chunk.h"
#include "Shader.h"
#include "World.h"

ChunkMesh::ChunkMesh() : VAO(0), VBO(0), vertexCount(0) {
    Initialize();
}

ChunkMesh::~ChunkMesh() {
    Clear();
}

ChunkMesh::ChunkMesh(const ChunkMesh& other) : VAO(0), VBO(0), vertexCount(other.vertexCount) {
    Initialize();
    //CopyFrom(other);
}

ChunkMesh& ChunkMesh::operator=(const ChunkMesh& other) { // Move
    if (this != &other) {
        Clear();
        Initialize();
        vertexCount = other.vertexCount;
        //CopyFrom(other);
    }
    return *this;
}

ChunkMesh::ChunkMesh(ChunkMesh&& other) noexcept : VAO(other.VAO), VBO(other.VBO), vertexCount(other.vertexCount) {
    other.VAO = 0;
    other.VBO = 0;
}

ChunkMesh& ChunkMesh::operator=(ChunkMesh&& other) noexcept { // Copy
    if (this != &other) {
        Clear();
        VAO = other.VAO;
        VBO = other.VBO;
        vertexCount = other.vertexCount;

        other.VAO = 0;
        other.VBO = 0;
    }
    return *this;
}

void ChunkMesh::Initialize() {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    //std::cout << "INIT" << std::endl;
}

void ChunkMesh::Clear() {
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

const int kFaceNeighborOffsets[6][3] = {
    { 1,  0,  0},  // Right face
    {-1,  0,  0},  // Left face
    { 0,  1,  0},  // Top face
    { 0, -1,  0},  // Bottom face
    { 0,  0,  1},  // Back face
    { 0,  0, -1},  // Front face
};

void ChunkMesh::GenerateMesh(const Chunk& chunk, World& world, int chunkX, int chunkY, int chunkZ) {
    //std::cout << "MESH GEN" << std::endl;
    std::vector<uint32_t> vertices;
    //std::vector<unsigned int> indices;

    for (int x = 0; x < Chunk::CHUNK_SIZE; ++x) {
        for (int y = 0; y < Chunk::CHUNK_SIZE; ++y) {
            for (int z = 0; z < Chunk::CHUNK_SIZE; ++z) {
                const Block& block = chunk.GetBlock(x, y, z);
                if (block.type != 0) {
                    for (int face = 0; face < 6; ++face) {
                        int neighborX = x + kFaceNeighborOffsets[face][0];
                        int neighborY = y + kFaceNeighborOffsets[face][1];
                        int neighborZ = z + kFaceNeighborOffsets[face][2];

                        bool neighborBlockCulls;
                        if (neighborX >= 0 && neighborX < Chunk::CHUNK_SIZE &&
                            neighborY >= 0 && neighborY < Chunk::CHUNK_SIZE &&
                            neighborZ >= 0 && neighborZ < Chunk::CHUNK_SIZE) {
                            // Neighbor is within the same chunk
                            neighborBlockCulls = chunk.GetBlockCulls(neighborX, neighborY, neighborZ);
                        }
                        else {
                            // Neighbor is in an adjacent chunk
                            auto globalNeighborX = chunkX * Chunk::CHUNK_SIZE + neighborX;
                            auto globalNeighborY = chunkY * Chunk::CHUNK_SIZE + neighborY;
                            auto globalNeighborZ = chunkZ * Chunk::CHUNK_SIZE + neighborZ;
                            neighborBlockCulls = world.GetBlockCulls(globalNeighborX, globalNeighborY, globalNeighborZ);
                        }

                        if (!neighborBlockCulls) {
                            //unsigned int indexOffset = static_cast<unsigned int>(vertices.size());

                            block.AddFaceVertices(vertices, face, x, y, z);
                            //block.GenerateFaceIndices(indices, indexOffset);
                        }
                    }
                }
            }
        }
    }

    //indexCount = static_cast<GLsizei>(indices.size());
    vertexCount = static_cast<GLsizei>(vertices.size());

    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(uint32_t), vertices.data(), GL_STATIC_DRAW);

    //if (vertices.size() > 0)
        //std::cout << vertices.size() << ", " << sizeof(uint32_t) << ", " << vertices.data()[4] << std::endl;

    //glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    //glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    //glVertexAttribPointer(0, 1, GL_UNSIGNED_INT, GL_FALSE, 0, (void*)0);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(uint32_t), (void*)0);
    //glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    //glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(0);
    //glEnableVertexAttribArray(1);
    //glEnableVertexAttribArray(2);

    glBindVertexArray(0);
}

void ChunkMesh::Render(Shader& shader) {
    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    glBindVertexArray(0);
}