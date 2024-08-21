#include "Chunk.h"
#include "Shader.h"
#include "World.h"
#include <iostream>

Chunk::Chunk() : world(0), chunkX(0), chunkY(0), chunkZ(0), isGenerated(false) {}

Chunk::Chunk(World* world, int chunkX, int chunkY, int chunkZ) : world(world), chunkX(chunkX), chunkY(chunkY), chunkZ(chunkZ), isGenerated(false) {
    blocks.fill(Block(BlockType::AIR));

    for (int x = 0; x < CHUNK_SIZE; x++)
    {
        for (int y = 0; y < CHUNK_SIZE; y++)
        {
            for (int z = 0; z < CHUNK_SIZE; z++)
            {
                int blockY = chunkY * CHUNK_SIZE + y;

                float worldHeight1 = world->terrainGenerator.GetHeight(chunkX * CHUNK_SIZE + x, chunkZ * CHUNK_SIZE + z + 1);
                float worldHeight2 = world->terrainGenerator.GetHeight(chunkX * CHUNK_SIZE + x + 1, chunkZ * CHUNK_SIZE + z);
                float worldHeight3 = world->terrainGenerator.GetHeight(chunkX * CHUNK_SIZE + x, chunkZ * CHUNK_SIZE + z);
                float worldHeight4 = world->terrainGenerator.GetHeight(chunkX * CHUNK_SIZE + x + 1, chunkZ * CHUNK_SIZE + z + 1);
                float fWorldHeight1 = worldHeight1 - blockY;
                float fWorldHeight2 = worldHeight2 - blockY;
                float fWorldHeight3 = worldHeight3 - blockY;
                float fWorldHeight4 = worldHeight4 - blockY;
                
                // Ceil just feels like a hacky workaround and doesn't address the actual issue
                int blockHeight1 = static_cast<int>(ceil(fWorldHeight1 * 8));
                int blockHeight2 = static_cast<int>(ceil(fWorldHeight2 * 8));
                int blockHeight3 = static_cast<int>(ceil(fWorldHeight3 * 8));
                int blockHeight4 = static_cast<int>(ceil(fWorldHeight4 * 8));
                
                float minHeight1 = worldHeight1 < worldHeight2 ? worldHeight1 : worldHeight2;
                float minHeight2 = worldHeight3 < worldHeight4 ? worldHeight3 : worldHeight4;
                int minHeight = static_cast<int>(floor(minHeight1 < minHeight2 ? minHeight1 : minHeight2));

                // This still creates ridges where there should be thin voxels

                if (blockY == minHeight) {
                    EdgeData edges = EdgeData();
                    edges.SetTopY(0, blockHeight1);
                    edges.SetTopY(1, blockHeight2);
                    edges.SetTopY(2, blockHeight3);
                    edges.SetTopY(3, blockHeight4);
                    SetBlock(x, y, z, BlockType::STONE, edges, false);
                }
                else if (blockY < minHeight) {
                    SetBlock(x, y, z, BlockType::STONE, false);
                }
            }
        }
    }

    //GenerateMesh(*world);
}

Chunk::~Chunk() {}

Chunk::Chunk(const Chunk& other) : blocks(), mesh(other.mesh), world(other.world),
chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ), isGenerated(other.isGenerated) {
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
        isGenerated = other.isGenerated;
        //GenerateMesh(*other.world);
    }
    //std::cout << "const Chunk& other" << std::endl;
    return *this;
}

Chunk::Chunk(Chunk&& other) noexcept : blocks(std::move(other.blocks)), mesh(std::move(other.mesh)),
world(other.world), chunkX(other.chunkX), chunkY(other.chunkY), chunkZ(other.chunkZ), isGenerated(other.isGenerated) {
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
        isGenerated = std::move(other.isGenerated);
        // No need to generate mesh, as it's moved
    }
    //std::cout << "Chunk&& other" << std::endl;
    return *this;
}

void Chunk::GenerateMesh(World& world) {
    mesh.GenerateMesh(*this, world, chunkX, chunkY, chunkZ);
    isGenerated = true;
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

void Chunk::SetBlock(int x, int y, int z, BlockType type, EdgeData edges, bool regenerateMesh) {
    blocks[Index(x, y, z)].type = type;
    blocks[Index(x, y, z)].SetEdgeData(edges);
    if (regenerateMesh)
        GenerateMesh(*world);
}