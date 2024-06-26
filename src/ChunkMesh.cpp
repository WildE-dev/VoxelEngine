#include <glm.hpp>
#include <ext/matrix_transform.hpp>
#include <iostream>

#include "ChunkMesh.h"
#include "Block.h"
#include "Chunk.h"
#include "Shader.h"
#include "World.h"

ChunkMesh::ChunkMesh() : VAO(0), VBO(0), EBO(0), indexCount(0) {
    Initialize();
}

ChunkMesh::~ChunkMesh() {
    Clear();
}

ChunkMesh::ChunkMesh(const ChunkMesh& other) : VAO(0), VBO(0), EBO(0), indexCount(other.indexCount) {
    Initialize();
    //CopyFrom(other);
}

ChunkMesh& ChunkMesh::operator=(const ChunkMesh& other) { // Move
    if (this != &other) {
        Clear();
        Initialize();
        indexCount = other.indexCount;
        //CopyFrom(other);
    }
    return *this;
}

ChunkMesh::ChunkMesh(ChunkMesh&& other) noexcept : VAO(other.VAO), VBO(other.VBO), EBO(other.EBO), indexCount(other.indexCount) {
    other.VAO = 0;
    other.VBO = 0;
    other.EBO = 0;
}

ChunkMesh& ChunkMesh::operator=(ChunkMesh&& other) noexcept { // Copy
    if (this != &other) {
        Clear();
        VAO = other.VAO;
        VBO = other.VBO;
        EBO = other.EBO;
        indexCount = other.indexCount;

        other.VAO = 0;
        other.VBO = 0;
        other.EBO = 0;
    }
    return *this;
}

void ChunkMesh::Initialize() {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    //std::cout << "INIT" << std::endl;
}

void ChunkMesh::Clear() {
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &EBO);
}

const float blockVertices[6][48] = {
    // +X
    {
        0.5f, -0.5f, -0.5f, 0.0f, 0.0f,  1.0f,  0.0f,  0.0f,
        0.5f,  0.5f,  0.5f, 1.0f, 1.0f,  1.0f,  0.0f,  0.0f,
        0.5f,  0.5f, -0.5f, 0.0f, 1.0f,  1.0f,  0.0f,  0.0f,

        0.5f, -0.5f, -0.5f, 0.0f, 0.0f,  1.0f,  0.0f,  0.0f,
        0.5f, -0.5f,  0.5f, 1.0f, 0.0f,  1.0f,  0.0f,  0.0f,
        0.5f,  0.5f,  0.5f, 1.0f, 1.0f,  1.0f,  0.0f,  0.0f,
    },

    // -X                                           
    {
         -0.5f, -0.5f,  0.5f, 0.0f, 0.0f, -1.0f,  0.0f,  0.0f,
         -0.5f,  0.5f, -0.5f, 1.0f, 1.0f, -1.0f,  0.0f,  0.0f,
         -0.5f,  0.5f,  0.5f, 0.0f, 1.0f, -1.0f,  0.0f,  0.0f,

         -0.5f, -0.5f,  0.5f, 0.0f, 0.0f, -1.0f,  0.0f,  0.0f,
         -0.5f, -0.5f, -0.5f, 1.0f, 0.0f, -1.0f,  0.0f,  0.0f,
         -0.5f,  0.5f, -0.5f, 1.0f, 1.0f, -1.0f,  0.0f,  0.0f,
    },

    // +Y                                           
    {
          0.5f,  0.5f, -0.5f, 0.0f, 1.0f,  0.0f,  1.0f,  0.0f,
          0.5f,  0.5f,  0.5f, 0.0f, 0.0f,  0.0f,  1.0f,  0.0f,
         -0.5f,  0.5f,  0.5f, 1.0f, 0.0f,  0.0f,  1.0f,  0.0f,

          0.5f,  0.5f, -0.5f, 0.0f, 1.0f,  0.0f,  1.0f,  0.0f,
         -0.5f,  0.5f,  0.5f, 1.0f, 0.0f,  0.0f,  1.0f,  0.0f,
         -0.5f,  0.5f, -0.5f, 1.0f, 1.0f,  0.0f,  1.0f,  0.0f,
    },

    // -Y
    {
          0.5f, -0.5f, -0.5f, 1.0f, 0.0f,  0.0f, -1.0f,  0.0f,
         -0.5f, -0.5f,  0.5f, 0.0f, 1.0f,  0.0f, -1.0f,  0.0f,
          0.5f, -0.5f,  0.5f, 1.0f, 1.0f,  0.0f, -1.0f,  0.0f,

          0.5f, -0.5f, -0.5f, 1.0f, 0.0f,  0.0f, -1.0f,  0.0f,
         -0.5f, -0.5f, -0.5f, 0.0f, 0.0f,  0.0f, -1.0f,  0.0f,
         -0.5f, -0.5f,  0.5f, 0.0f, 1.0f,  0.0f, -1.0f,  0.0f,
    },

    // +Z
    {
           0.5f, -0.5f,  0.5f, 1.0f, 0.0f,  0.0f,  0.0f,  1.0f,
          -0.5f,  0.5f,  0.5f, 0.0f, 1.0f,  0.0f,  0.0f,  1.0f,
           0.5f,  0.5f,  0.5f, 1.0f, 1.0f,  0.0f,  0.0f,  1.0f,

           0.5f, -0.5f,  0.5f, 1.0f, 0.0f,  0.0f,  0.0f,  1.0f,
          -0.5f, -0.5f,  0.5f, 0.0f, 0.0f,  0.0f,  0.0f,  1.0f,
          -0.5f,  0.5f,  0.5f, 0.0f, 1.0f,  0.0f,  0.0f,  1.0f,
    },

    // -Z
    {
          -0.5f, -0.5f, -0.5f, 1.0f, 0.0f,  0.0f,  0.0f, -1.0f,
           0.5f,  0.5f, -0.5f, 0.0f, 1.0f,  0.0f,  0.0f, -1.0f,
          -0.5f,  0.5f, -0.5f, 1.0f, 1.0f,  0.0f,  0.0f, -1.0f,

          -0.5f, -0.5f, -0.5f, 1.0f, 0.0f,  0.0f,  0.0f, -1.0f,
           0.5f, -0.5f, -0.5f, 0.0f, 0.0f,  0.0f,  0.0f, -1.0f,
           0.5f,  0.5f, -0.5f, 0.0f, 1.0f,  0.0f,  0.0f, -1.0f,
    }
};
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
    std::vector<float> vertices;
    std::vector<unsigned int> indices;

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
                            unsigned int indexOffset = static_cast<unsigned int>(vertices.size() / 8);

                            block.AddFaceVertices(vertices, face, x, y, z);
                            block.GenerateFaceIndices(indices, indexOffset);
                        }
                    }
                }
            }
        }
    }

    indexCount = static_cast<GLsizei>(indices.size());

    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
}

void ChunkMesh::Render(Shader& shader) {
    glBindVertexArray(VAO);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}