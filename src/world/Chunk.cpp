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
    return data ? data->blocks[Index(x, y, z)] : Block();
}

void Chunk::CopyBlocks(Blocks& out) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    if (data) out = data->blocks;
    else out.fill(Block());
}

bool Chunk::IsOpaque(int x, int y, int z) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    return data && data->blocks[Index(x, y, z)].IsFullBlock();
}

void Chunk::SetBlock(int x, int y, int z, const Block& block) {
    std::lock_guard<std::mutex> lock(blockMutex);
    DataLocked().blocks[Index(x, y, z)] = block;
}

bool Chunk::TryBeginGenerate() {
    State expected = State::Pending;
    return state.compare_exchange_strong(expected, State::Generating);
}

void Chunk::SetGenerated(const Blocks& generated) {
    {
        std::lock_guard<std::mutex> lock(blockMutex);
        DataLocked().blocks = generated;
    }
    state = State::Loaded;
}

bool Chunk::PublishLight() {
    {
        std::lock_guard<std::mutex> lock(blockMutex);
        Contents& d = DataLocked();
        if (d.publishedLight == d.light && lightVersion.load(std::memory_order_relaxed) > 0) return false;
        d.publishedLight = d.light;
    }
    lightVersion.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void Chunk::CopyPublishedLight(Light& out) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    if (data) out = data->publishedLight;
    else out.fill(0);
}

uint8_t Chunk::GetReceivedLight(int x, int y, int z) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    return data ? data->light[Index(x, y, z)] : 0;
}

void Chunk::SetLightData(const Light& values, std::vector<int>* changedCells) {
    std::lock_guard<std::mutex> lock(blockMutex);
    Contents& d = DataLocked();
    if (changedCells) {
        changedCells->clear();
        for (int i = 0; i < VOLUME; i++)
            if (d.light[i] != values[i]) changedCells->push_back(i);
    }
    d.light = values;
}
