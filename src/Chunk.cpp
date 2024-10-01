#include "Chunk.h"
#include "Shader.h"
#include "World.h"
#include <iostream>

//Chunk::Chunk() : world(0), chunkX(0), chunkY(0), chunkZ(0), isGenerated(false) {}

Chunk::Chunk(World* world, int chunkX, int chunkY, int chunkZ) : world(world), 
chunkX(chunkX), chunkY(chunkY), chunkZ(chunkZ), isSetup(false), isLoaded(false), 
VAO(0), VBO(0), isEmpty(false), isFull(false), isSurrounded(false), needsRebuilding(false) {
    Initialize();
    chunkCount++;
}

Chunk::~Chunk() {
    Clear();
    chunkCount--;
}

Chunk::Chunk(const Chunk& other) : blocks(), world(other.world), isSetup(other.isSetup),
chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ), VAO(other.VAO), VBO(other.VBO), isEmpty(other.isEmpty), 
isFull(other.isFull), isSurrounded(other.isSurrounded), needsRebuilding(other.needsRebuilding) {}

Chunk& Chunk::operator=(const Chunk& other) {
    if (this != &other) {
        blocks = other.blocks;
        world = other.world;
        chunkX = other.chunkX;
        chunkY = other.chunkY;
        chunkZ = other.chunkZ;
        VAO = other.VAO;
        VBO = other.VBO;
    }
    return *this;
}

Chunk::Chunk(Chunk&& other) noexcept : blocks(std::move(other.blocks)), isSetup(other.isSetup),
world(other.world), chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ), VAO(other.VAO), VBO(other.VBO), 
isEmpty(other.isEmpty), isFull(other.isFull), isSurrounded(other.isSurrounded), needsRebuilding(other.needsRebuilding) {}

Chunk& Chunk::operator=(Chunk&& other) noexcept {
    if (this != &other) {
        blocks = std::move(other.blocks);
        world = std::move(other.world);
        chunkX = std::move(other.chunkX);
        chunkY = std::move(other.chunkY);
        chunkZ = std::move(other.chunkZ);
        VAO = std::move(other.VAO);
        VBO = std::move(other.VBO);
    }
    return *this;
}

void Chunk::LoadChunk(TerrainGenerator* terrainGenerator) {
    std::lock_guard<std::mutex> lock(block_mutex);
    blocks.fill(Block(BlockType::AIR));

    float heights[CHUNK_SIZE + 1][CHUNK_SIZE + 1];

    for (int x = 0; x < CHUNK_SIZE + 1; x++) {
        for (int z = 0; z < CHUNK_SIZE + 1; z++) {
            int blockX = chunkX * CHUNK_SIZE + x;
            int blockZ = chunkZ * CHUNK_SIZE + z;
            heights[x][z] = terrainGenerator->GetHeight(blockX, blockZ);
        }
    }

    for (int x = 0; x < CHUNK_SIZE; x++)
    {
        for (int y = 0; y < CHUNK_SIZE; y++)
        {
            for (int z = 0; z < CHUNK_SIZE; z++)
            {
                int blockY = chunkY * CHUNK_SIZE + y;

                const float smoothness = 8.0f;

                float worldHeight1 = round(heights[x][z + 1] * smoothness) / smoothness;
                float worldHeight2 = round(heights[x + 1][z] * smoothness) / smoothness;
                float worldHeight3 = round(heights[x][z] * smoothness) / smoothness;
                float worldHeight4 = round(heights[x + 1][z + 1] * smoothness) / smoothness;
                
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
                    SetBlock(x, y, z, BlockType::GRASS, edges);
                }
                else if (blockY < minHeight && blockY > minHeight - 5) {
                    SetBlock(x, y, z, BlockType::DIRT);
                }
                else if (blockY < minHeight) {
                    SetBlock(x, y, z, BlockType::STONE);
                }
            }
        }
    }

    isLoaded = true;
}

void Chunk::SetupChunk()
{
    needsRebuilding = true;
    isSetup = true;
}

void Chunk::UnloadChunk()
{
    isLoaded = false;
}

int Chunk::Index(int x, int y, int z) const {
    return x + CHUNK_SIZE * (y + CHUNK_SIZE * z);
}

const Block& Chunk::GetBlock(int x, int y, int z) {
    std::lock_guard<std::mutex> lock(block_mutex);
    return blocks[Index(x, y, z)];
}

bool Chunk::ShouldRender()
{
    return vertices.size() > 0;
}

bool Chunk::IsLoaded() const
{
    return isLoaded;
}

bool Chunk::IsSetup() const
{
    return isSetup;
}

bool Chunk::NeedsRebuilding() const
{
    return needsRebuilding;
}

bool Chunk::IsEmpty() const
{
    return isEmpty;
}

bool Chunk::IsFull() const
{
    return isFull;
}

bool Chunk::IsSurrounded() const
{
    return isSurrounded;
}

void Chunk::SetIsSurrounded(bool value)
{
    isSurrounded = value;
}

void Chunk::SetNeedsRebuilding(bool value)
{
    needsRebuilding = value;
}

void Chunk::UpdateEmptyFullFlags()
{
    isEmpty = true;
    isFull = true;

    for (int x = 0; x < CHUNK_SIZE; x++)
    {
        for (int y = 0; y < CHUNK_SIZE; y++)
        {
            for (int z = 0; z < CHUNK_SIZE; z++)
            {
                auto& block = GetBlock(x, y, z);
                if (block.IsFullBlock()) {
                    isEmpty = false;
                }
                else {
                    isFull = false;
                }

                if (!isEmpty && !isFull) return; // Since we know it's not full and not empty we don't need to check the rest of the blocks
            }
        }
    }
}

glm::ivec3 Chunk::GetCoords()
{
    return glm::ivec3(chunkX, chunkY, chunkZ);
}

bool Chunk::GetBlockCulls(int x, int y, int z)
{
    std::lock_guard<std::mutex> lock(block_mutex);
    Block b = blocks[Index(x, y, z)];

    return b.IsFullBlock();
}

void Chunk::SetBlock(int x, int y, int z, Block block)
{
    //std::lock_guard<std::mutex> lock(block_mutex);
    blocks[Index(x, y, z)] = block;
}

void Chunk::SetBlock(int x, int y, int z, BlockType type) {
    //std::lock_guard<std::mutex> lock(block_mutex);
    blocks[Index(x, y, z)].type = type;
    blocks[Index(x, y, z)].edgeData.MakeFull();
}

void Chunk::SetBlock(int x, int y, int z, EdgeData edges) {
    //std::lock_guard<std::mutex> lock(block_mutex);
    blocks[Index(x, y, z)].SetEdgeData(edges);
}

void Chunk::SetBlock(int x, int y, int z, BlockType type, EdgeData edges) {
    //std::lock_guard<std::mutex> lock(block_mutex);
    blocks[Index(x, y, z)].type = type;
    blocks[Index(x, y, z)].SetEdgeData(edges);
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

    needsRebuilding = false;

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
    if (!isLoaded)
        return;

    /*if (!meshSentToGPU) {
        SendVertexData();
    }*/

    shader.Use();

    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 2));
    glBindVertexArray(0);
}