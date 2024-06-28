#include "Chunk.h"
#include "Shader.h"
#include <iostream>

Chunk::Chunk() : world(0), chunkX(0), chunkY(0), chunkZ(0) {}

Chunk::Chunk(World* world, int chunkX, int chunkY, int chunkZ) : world(world), chunkX(chunkX), chunkY(chunkY), chunkZ(chunkZ) {
    blocks.fill(Block(BlockType::STONE));

    for (size_t x = 0; x < CHUNK_SIZE; x++)
    {
        for (size_t y = 11; y < CHUNK_SIZE; y++)
        {
            for (size_t z = 0; z < CHUNK_SIZE; z++)
            {
                if (y < CHUNK_SIZE - 1)
                    SetBlock(x, y, z, BlockType::DIRT, false);
                else
                    SetBlock(x, y, z, BlockType::GRASS, false);
            }
        }
    }

    //GenerateMesh(*world);
}

Chunk::~Chunk() {}

Chunk::Chunk(const Chunk& other) : blocks(), mesh(other.mesh), world(other.world),
chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ) {
    //GenerateMesh(*other.world);
}

Chunk& Chunk::operator=(const Chunk& other) {
    if (this != &other) {
        blocks = other.blocks;
        mesh = other.mesh;
        world = other.world;
        chunkX = other.chunkX;
        chunkY = other.chunkY;
        chunkZ = other.chunkZ;
        //GenerateMesh(*other.world);
    }
    //std::cout << "const Chunk& other" << std::endl;
    return *this;
}

Chunk::Chunk(Chunk&& other) noexcept : blocks(std::move(other.blocks)), mesh(std::move(other.mesh)),
world(other.world), chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ) {
    // No need to generate mesh, as it's moved
}

Chunk& Chunk::operator=(Chunk&& other) noexcept {
    if (this != &other) {
        blocks = std::move(other.blocks);
        mesh = std::move(other.mesh);
        world = std::move(other.world);
        chunkX = std::move(other.chunkX);
        chunkY = std::move(other.chunkY);
        chunkZ = std::move(other.chunkZ);
        // No need to generate mesh, as it's moved
    }
    //std::cout << "Chunk&& other" << std::endl;
    return *this;
}

void Chunk::GenerateMesh(World& world) {
    mesh.GenerateMesh(*this, world, chunkX, chunkY, chunkZ);
}

void Chunk::Render(Shader& shader) {
    mesh.Render(shader);
}

int Chunk::Index(int x, int y, int z) const {
    return x + CHUNK_SIZE * (y + CHUNK_SIZE * z);
}

const Block& Chunk::GetBlock(int x, int y, int z) const {
    return blocks[Index(x, y, z)];
}

bool Chunk::GetBlockCulls(int x, int y, int z) const
{
    Block b = blocks[Index(x, y, z)];

    if (b.type == BlockType::AIR)
        return false;

    return b.IsFullBlock();
}

void Chunk::SetBlock(int x, int y, int z, Block block, bool regenerateMesh)
{
    blocks[Index(x, y, z)] = block;
    if (regenerateMesh)
        GenerateMesh(*world);
}

void Chunk::SetBlock(int x, int y, int z, BlockType type, bool regenerateMesh) {
    blocks[Index(x, y, z)].type = type;
    if (regenerateMesh)
        GenerateMesh(*world);
}

void Chunk::SetBlock(int x, int y, int z, EdgeData edges, bool regenerateMesh) {
    blocks[Index(x, y, z)].SetEdgeData(edges);
    if (regenerateMesh)
        GenerateMesh(*world);
}