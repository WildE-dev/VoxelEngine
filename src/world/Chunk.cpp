#include "world/Chunk.h"

#include <algorithm>
#include <cmath>


std::atomic<int> Chunk::liveCount{0};

Chunk::Chunk(glm::ivec3 coords) : coords(coords) {
    liveCount++;
}

Chunk::~Chunk() {
    liveCount--;
}

Block Chunk::GetBlock(int x, int y, int z) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    return blocks[Index(x, y, z)];
}

void Chunk::CopyBlocks(Blocks& out) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    out = blocks;
}

bool Chunk::IsOpaque(int x, int y, int z) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    return blocks[Index(x, y, z)].IsFullBlock();
}

void Chunk::SetBlock(int x, int y, int z, const Block& block) {
    std::lock_guard<std::mutex> lock(blockMutex);
    blocks[Index(x, y, z)] = block;
}

bool Chunk::TryBeginGenerate() {
    State expected = State::Pending;
    return state.compare_exchange_strong(expected, State::Generating);
}

void Chunk::SetGenerated(const Blocks& generated) {
    {
        std::lock_guard<std::mutex> lock(blockMutex);
        blocks = generated;
    }
    state = State::Loaded;
}

bool Chunk::PublishLight() {
    {
        std::lock_guard<std::mutex> lock(blockMutex);
        if (publishedLight == light && lightVersion.load(std::memory_order_relaxed) > 0) return false;
        publishedLight = light;
    }
    lightVersion.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void Chunk::CopyPublishedLight(Light& out) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    out = publishedLight;
}

void Chunk::SetLightData(const Light& values, std::vector<int>* changedCells) {
    std::lock_guard<std::mutex> lock(blockMutex);
    if (changedCells) {
        changedCells->clear();
        for (int i = 0; i < VOLUME; i++)
            if (light[i] != values[i]) changedCells->push_back(i);
    }
    light = values;
}
