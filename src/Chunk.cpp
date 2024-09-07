#include "Chunk.h"
#include "Shader.h"
#include "World.h"
#include <iostream>

//Chunk::Chunk() : world(0), chunkX(0), chunkY(0), chunkZ(0), isGenerated(false) {}

Chunk::Chunk(World* world, int chunkX, int chunkY, int chunkZ) : world(world), chunkX(chunkX), chunkY(chunkY), chunkZ(chunkZ), isGenerated(false), VAO(0), VBO(0) {
    Initialize();
}

Chunk::~Chunk() {
    Clear();
}

Chunk::Chunk(const Chunk& other) : blocks(), world(other.world),
chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ), isGenerated(other.isGenerated), VAO(other.VAO), VBO(other.VBO) {}

Chunk& Chunk::operator=(const Chunk& other) {
    if (this != &other) {
        blocks = other.blocks;
        world = other.world;
        chunkX = other.chunkX;
        chunkY = other.chunkY;
        chunkZ = other.chunkZ;
        isGenerated = other.isGenerated;
        VAO = other.VAO;
        VBO = other.VBO;
    }
    return *this;
}

Chunk::Chunk(Chunk&& other) noexcept : blocks(std::move(other.blocks)),
world(other.world), chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ), isGenerated(other.isGenerated), VAO(other.VAO), VBO(other.VBO) {}

Chunk& Chunk::operator=(Chunk&& other) noexcept {
    if (this != &other) {
        blocks = std::move(other.blocks);
        world = std::move(other.world);
        chunkX = std::move(other.chunkX);
        chunkY = std::move(other.chunkY);
        chunkZ = std::move(other.chunkZ);
        isGenerated = std::move(other.isGenerated);
        VAO = std::move(other.VAO);
        VBO = std::move(other.VBO);
    }
    return *this;
}

void Chunk::LoadChunk() {
    blocks.fill(Block(BlockType::AIR));

    for (int x = 0; x < CHUNK_SIZE; x++)
    {
        for (int y = 0; y < CHUNK_SIZE; y++)
        {
            for (int z = 0; z < CHUNK_SIZE; z++)
            {
                int blockX = chunkX * CHUNK_SIZE + x;
                int blockY = chunkY * CHUNK_SIZE + y;
                int blockZ = chunkZ * CHUNK_SIZE + z;

                const float smoothness = 8.0f;

                float worldHeight1 = round(world->terrainGenerator->GetHeight(blockX, blockZ + 1) * smoothness) / smoothness;
                float worldHeight2 = round(world->terrainGenerator->GetHeight(blockX + 1, blockZ) * smoothness) / smoothness;
                float worldHeight3 = round(world->terrainGenerator->GetHeight(blockX, blockZ) * smoothness) / smoothness;
                float worldHeight4 = round(world->terrainGenerator->GetHeight(blockX + 1, blockZ + 1) * smoothness) / smoothness;
                float fWorldHeight1 = worldHeight1 - blockY;
                float fWorldHeight2 = worldHeight2 - blockY;
                float fWorldHeight3 = worldHeight3 - blockY;
                float fWorldHeight4 = worldHeight4 - blockY;

                int blockHeight1 = static_cast<int>(fWorldHeight1 * 8);
                int blockHeight2 = static_cast<int>(fWorldHeight2 * 8);
                int blockHeight3 = static_cast<int>(fWorldHeight3 * 8);
                int blockHeight4 = static_cast<int>(fWorldHeight4 * 8);

                float minHeight1 = worldHeight1 < worldHeight2 ? worldHeight1 : worldHeight2;
                float minHeight2 = worldHeight3 < worldHeight4 ? worldHeight3 : worldHeight4;
                int minHeight = static_cast<int>(floor(minHeight1 < minHeight2 ? minHeight1 : minHeight2));

                if (blockY == minHeight) {
                    EdgeData edges = EdgeData();
                    edges.SetTopY(0, blockHeight1);
                    edges.SetTopY(1, blockHeight2);
                    edges.SetTopY(2, blockHeight3);
                    edges.SetTopY(3, blockHeight4);
                    SetBlock(x, y, z, BlockType::STONE, edges);
                }
                else if (blockY < minHeight) {
                    SetBlock(x, y, z, BlockType::STONE);
                }
            }
        }
    }

    isLoaded = true;
}

int Chunk::Index(int x, int y, int z) const {
    return x + CHUNK_SIZE * (y + CHUNK_SIZE * z);
}

const Block& Chunk::GetBlock(int x, int y, int z) {
    return blocks[Index(x, y, z)];
}

bool Chunk::ShouldGenerateMesh()
{
    return !isGenerated && isLoaded;
}

bool Chunk::GetIsGenerated()
{
    return isGenerated;
}

void Chunk::SetIsGenerated(bool value)
{
    isGenerated = value;
}

std::tuple<int, int, int> Chunk::GetCoords()
{
    return std::make_tuple(chunkX, chunkY, chunkZ);
}

bool Chunk::GetBlockCulls(int x, int y, int z)
{
    Block b = blocks[Index(x, y, z)];

    if (b.type == BlockType::AIR)
        return false;

    return b.IsFullBlock();
}

void Chunk::SetBlock(int x, int y, int z, Block block)
{
    blocks[Index(x, y, z)] = block;
    isGenerated = false;
}

void Chunk::SetBlock(int x, int y, int z, BlockType type) {
    blocks[Index(x, y, z)].type = type;
    isGenerated = false;
}

void Chunk::SetBlock(int x, int y, int z, EdgeData edges) {
    blocks[Index(x, y, z)].SetEdgeData(edges);
    isGenerated = false;
}

void Chunk::SetBlock(int x, int y, int z, BlockType type, EdgeData edges) {
    blocks[Index(x, y, z)].type = type;
    blocks[Index(x, y, z)].SetEdgeData(edges);
    isGenerated = false;
}

void Chunk::Initialize() {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
}

void Chunk::Clear() {
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

void Chunk::GenerateMesh() {
    vertices.clear();
    for (int x = 0; x < Chunk::CHUNK_SIZE; ++x) {
        for (int y = 0; y < Chunk::CHUNK_SIZE; ++y) {
            for (int z = 0; z < Chunk::CHUNK_SIZE; ++z) {
                const Block& block = GetBlock(x, y, z);
                if (block.type != BlockType::AIR) {
                    for (int face = 0; face < 6; ++face) {
                        int neighborX = x + kFaceNeighborOffsets[face][0];
                        int neighborY = y + kFaceNeighborOffsets[face][1];
                        int neighborZ = z + kFaceNeighborOffsets[face][2];

                        bool neighborBlockCulls;
                        if (neighborX >= 0 && neighborX < Chunk::CHUNK_SIZE &&
                            neighborY >= 0 && neighborY < Chunk::CHUNK_SIZE &&
                            neighborZ >= 0 && neighborZ < Chunk::CHUNK_SIZE) {
                            // Neighbor is within the same chunk
                            neighborBlockCulls = GetBlockCulls(neighborX, neighborY, neighborZ);
                        }
                        else {
                            // Neighbor is in an adjacent chunk
                            auto globalNeighborX = chunkX * Chunk::CHUNK_SIZE + neighborX;
                            auto globalNeighborY = chunkY * Chunk::CHUNK_SIZE + neighborY;
                            auto globalNeighborZ = chunkZ * Chunk::CHUNK_SIZE + neighborZ;
                            neighborBlockCulls = world->GetBlockCulls(globalNeighborX, globalNeighborY, globalNeighborZ);
                        }

                        if (!neighborBlockCulls || !block.IsFullBlock()) {
                            block.AddFaceVertices(vertices, face, x, y, z);
                        }
                    }
                }
            }
        }
    }

    isGenerated = true;
    SendVertexData();
}

void Chunk::SendVertexData() {
    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(uint32_t), vertices.data(), GL_STATIC_DRAW);

    //if (vertices.size() > 0)
        //std::cout << vertices.size() << ", " << sizeof(uint32_t) << ", " << vertices.data()[4] << std::endl;

    //glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    //glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    //glVertexAttribPointer(0, 1, GL_UNSIGNED_INT, GL_FALSE, 0, (void*)0);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 2 * sizeof(uint32_t), (void*)0);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, 2 * sizeof(uint32_t), (void*)sizeof(uint32_t));
    //glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    //glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    //glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    //meshSentToGPU = true;
}

void Chunk::Render(Shader& shader) {
    assert(isGenerated && isLoaded);

    /*if (!meshSentToGPU) {
        SendVertexData();
    }*/

    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
    glBindVertexArray(0);
}