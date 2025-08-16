#include "Chunk.h"
#include "Shader.h"
#include "World.h"
#include <iostream>

//Chunk::Chunk() : world(0), chunkX(0), chunkY(0), chunkZ(0), isGenerated(false) {}

Chunk::Chunk(World* world, int chunkX, int chunkY, int chunkZ)
    : world(world), chunkX(chunkX), chunkY(chunkY), chunkZ(chunkZ),
    isSetup(false), isLoaded(false), isInitialized(false), isMeshSent(false),
    VAO(0), VBO(0), isEmpty(false), isFull(false), isSurrounded(false),
    needsRebuilding(false) {
    chunkCount++;
}

Chunk::~Chunk() {
    Clear();
    chunkCount--;
}

Chunk::Chunk(const Chunk& other) : blocks(), world(other.world),
chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ), VAO(other.VAO), VBO(other.VBO), isEmpty(other.isEmpty), 
isFull(other.isFull), isSurrounded(other.isSurrounded) {}

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

Chunk::Chunk(Chunk&& other) noexcept : blocks(std::move(other.blocks)),
world(other.world), chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ), VAO(other.VAO), VBO(other.VBO), 
isEmpty(other.isEmpty), isFull(other.isFull), isSurrounded(other.isSurrounded) {}

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

void Chunk::Reset(int chunkX, int chunkY, int chunkZ) {
    std::lock_guard<std::mutex> lock(block_mutex);

    // Clear all block data
    blocks.fill(Block(BlockType::AIR));

    // Clear mesh data
    vertices.clear();
    vertex_count = 0;

    // Reset all flags
    isLoaded = false;
    isMeshSent = false;
    isSetup = false;
    needsRebuilding = false;
    isEmpty = false;
    isFull = false;
    isSurrounded = false;

    // Set new coordinates
    this->chunkX = chunkX;
    this->chunkY = chunkY;
    this->chunkZ = chunkZ;

    // Don't reset OpenGL buffers here - they'll be reused
    // Just mark that mesh needs to be sent again
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
        for (int z = 0; z < CHUNK_SIZE; z++)
        {
            const float smoothness = 8.0f;

            float worldHeight1 = round(heights[x][z + 1] * smoothness) / smoothness;
            float worldHeight2 = round(heights[x + 1][z] * smoothness) / smoothness;
            float worldHeight3 = round(heights[x][z] * smoothness) / smoothness;
            float worldHeight4 = round(heights[x + 1][z + 1] * smoothness) / smoothness;

            float minHeight1 = worldHeight1 < worldHeight2 ? worldHeight1 : worldHeight2;
            float minHeight2 = worldHeight3 < worldHeight4 ? worldHeight3 : worldHeight4;
            int minHeight = static_cast<int>(floor(minHeight1 < minHeight2 ? minHeight1 : minHeight2));

            for (int y = 0; y < CHUNK_SIZE; y++)
            {
                int blockY = chunkY * CHUNK_SIZE + y;

                float fWorldHeight1 = worldHeight1 - blockY;
                float fWorldHeight2 = worldHeight2 - blockY;
                float fWorldHeight3 = worldHeight3 - blockY;
                float fWorldHeight4 = worldHeight4 - blockY;

                int blockHeight1 = static_cast<int>(fWorldHeight1 * 8);
                int blockHeight2 = static_cast<int>(fWorldHeight2 * 8);
                int blockHeight3 = static_cast<int>(fWorldHeight3 * 8);
                int blockHeight4 = static_cast<int>(fWorldHeight4 * 8);

                if (blockY == minHeight) {
                    EdgeData edges = EdgeData();
                    edges.SetTopY(0, blockHeight1);
                    edges.SetTopY(1, blockHeight2);
                    edges.SetTopY(2, blockHeight3);
                    edges.SetTopY(3, blockHeight4);

                    if (edges.IsValid()) {
                        SetBlock(x, y, z, BlockType::GRASS, edges);
                    }
                    else {
                        if (y > 0) // Workaround so it doesn't set a block at a negative y
                            SetBlock(x, y - 1, z, BlockType::GRASS);
                    }
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
    std::lock_guard<std::mutex> lock(block_mutex);
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
    return vertex_count > 0;
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

void Chunk::UpdateChunkSurroundedFlag() {
    int offsets[][3] = {
                { 1, 0, 0 }, {-1, 0, 0 },
                { 0, 1, 0 }, { 0,-1, 0 },
                { 0, 0, 1 }, { 0, 0,-1 },
    };

    bool surrounded = true;
    for (int i = 0; i < 6; i++) {
        auto coords = GetCoords();
        auto neighbour = Chunk::world->GetChunk(coords.x + offsets[i][0], coords.y + offsets[i][1], coords.z + offsets[i][2]);
        if (neighbour == nullptr || !neighbour->IsFull()) {
            surrounded = false;
            break;
        }
    }
    SetIsSurrounded(surrounded);
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

void Chunk::InitializeMeshBuffers() {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
	isInitialized = true;
}

void Chunk::Clear() {
    vertices.clear();

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    isInitialized = false;
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
    // Early exit if already generating
    if (isGeneratingMesh.exchange(true)) {
        return; // Another thread is already generating this mesh
    }

    // Quick check for empty chunks
    bool hasBlocks = false;
    for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE; ++i) {
        if (blocks[i].type != BlockType::AIR) {
            hasBlocks = true;
            break;
        }
    }

    if (!hasBlocks) {
        vertices.clear();
        vertex_count = 0;
        needsRebuilding = false;
        isMeshSent = false;
        hasVisibleFaces = false;
        isGeneratingMesh = false;
        return;
    }

    // Cache neighbor chunks if needed
    if (!neighborCacheValid) {
        cachedNeighbors[0] = world->GetChunk(chunkX + 1, chunkY, chunkZ);
        cachedNeighbors[1] = world->GetChunk(chunkX - 1, chunkY, chunkZ);
        cachedNeighbors[2] = world->GetChunk(chunkX, chunkY + 1, chunkZ);
        cachedNeighbors[3] = world->GetChunk(chunkX, chunkY - 1, chunkZ);
        cachedNeighbors[4] = world->GetChunk(chunkX, chunkY, chunkZ + 1);
        cachedNeighbors[5] = world->GetChunk(chunkX, chunkY, chunkZ - 1);
        neighborCacheValid = true;
    }

    std::vector<uint32_t> new_vertices;
    new_vertices.reserve(vertices.size() > 0 ? vertices.size() : 1024); // Reserve space

    bool foundVisibleFaces = false;

    // Optimized face checking with early exits
    for (int x = 0; x < Chunk::CHUNK_SIZE; ++x) {
        for (int y = 0; y < Chunk::CHUNK_SIZE; ++y) {
            for (int z = 0; z < Chunk::CHUNK_SIZE; ++z) {
                const Block& block = blocks[Index(x, y, z)];
                if (block.type == BlockType::AIR) continue;

                // Check each face with optimized neighbor lookup
                for (int face = 0; face < 6; ++face) {
                    int neighborX = x + kFaceNeighborOffsets[face][0];
                    int neighborY = y + kFaceNeighborOffsets[face][1];
                    int neighborZ = z + kFaceNeighborOffsets[face][2];

                    bool neighborBlockCulls = false;

                    if (neighborX >= 0 && neighborX < Chunk::CHUNK_SIZE &&
                        neighborY >= 0 && neighborY < Chunk::CHUNK_SIZE &&
                        neighborZ >= 0 && neighborZ < Chunk::CHUNK_SIZE) {
                        // Internal neighbor - direct access
                        neighborBlockCulls = blocks[Index(neighborX, neighborY, neighborZ)].IsFullBlock();
                    }
                    else {
                        // External neighbor - use cached chunks
                        Chunk* neighborChunk = cachedNeighbors[face];
                        if (neighborChunk && neighborChunk->IsLoaded()) {
                            int nx = (neighborX + Chunk::CHUNK_SIZE) % Chunk::CHUNK_SIZE;
                            int ny = (neighborY + Chunk::CHUNK_SIZE) % Chunk::CHUNK_SIZE;
                            int nz = (neighborZ + Chunk::CHUNK_SIZE) % Chunk::CHUNK_SIZE;
                            neighborBlockCulls = neighborChunk->GetBlockCulls(nx, ny, nz);
                        }
                    }

                    if (!neighborBlockCulls || !block.IsFullBlock()) {
                        block.AddFaceVertices(new_vertices, face, x, y, z);
                        foundVisibleFaces = true;
                    }
                }
            }
        }
    }

    vertices = std::move(new_vertices);
    vertex_count = vertices.size();
    hasVisibleFaces = foundVisibleFaces;
    needsRebuilding = false;
    isMeshSent = false;
    isGeneratingMesh = false;
}

void Chunk::Render(Shader& shader) {
    if (!isLoaded || vertex_count == 0)
        return;

    if (!isInitialized) {
        InitializeMeshBuffers();
    }

    if (!isMeshSent && vertices.size() > 0) {
        SendVertexData();
    }

    shader.Use();

    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertex_count / 2));
}

void Chunk::SendVertexData() {
    if (vertices.empty()) return;

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(uint32_t), vertices.data(), GL_STATIC_DRAW);

    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 2 * sizeof(uint32_t), (void*)0);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, 2 * sizeof(uint32_t), (void*)sizeof(uint32_t));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    isMeshSent = true;
}