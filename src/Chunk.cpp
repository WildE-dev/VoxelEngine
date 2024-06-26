#include "Chunk.h"
#include "Shader.h"
#include <iostream>

Chunk::Chunk() : world(0), chunkX(0), chunkY(0), chunkZ(0) {}

Chunk::Chunk(World* world, int chunkX, int chunkY, int chunkZ) : world(world), chunkX(chunkX), chunkY(chunkY), chunkZ(chunkZ) {
    blocks.fill(Block(1));

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
    std::cout << chunkX << ", " << chunkY << ", " << chunkZ << std::endl;
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

    if (b.type == 0)
        return false;

    for (size_t i = 0; i < 4; i++)
    {
        if (b.edgeData.edges[i] != 0x80)
            return false;
    }

    return true;
}

void Chunk::SetBlock(int x, int y, int z, unsigned char type) {
    blocks[Index(x, y, z)] = Block(type);
    GenerateMesh(*world);
}