#pragma once

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include "world/Chunk.h"

// Integer helpers for converting between world, chunk and chunk-local coordinates.
namespace Coords {

inline int FloorDiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

inline int FloorMod(int a, int b) {
    return ((a % b) + b) % b;
}

inline glm::ivec3 ChunkOf(const glm::ivec3& world) {
    constexpr int S = Chunk::CHUNK_SIZE;
    return { FloorDiv(world.x, S), FloorDiv(world.y, S), FloorDiv(world.z, S) };
}

inline glm::ivec3 LocalOf(const glm::ivec3& world) {
    constexpr int S = Chunk::CHUNK_SIZE;
    return { FloorMod(world.x, S), FloorMod(world.y, S), FloorMod(world.z, S) };
}

// Meshes sample a one block border around their chunk, so a change to a block on a
// chunk face, edge or corner also affects up to seven neighbouring chunks. Calls
// fn(chunkCoords) for the block's own chunk and every such neighbour.
template <typename Fn>
void ForEachChunkTouching(const glm::ivec3& world, Fn fn) {
    const glm::ivec3 chunk = ChunkOf(world);
    const glm::ivec3 local = LocalOf(world);
    const int last = Chunk::CHUNK_SIZE - 1;
    glm::ivec3 lo, hi;
    for (int axis = 0; axis < 3; axis++) {
        lo[axis] = local[axis] == 0 ? -1 : 0;
        hi[axis] = local[axis] == last ? 1 : 0;
    }
    for (int dx = lo.x; dx <= hi.x; dx++)
        for (int dy = lo.y; dy <= hi.y; dy++)
            for (int dz = lo.z; dz <= hi.z; dz++)
                fn(chunk + glm::ivec3(dx, dy, dz));
}

struct IVec3Hash {
    size_t operator()(const glm::ivec3& v) const noexcept {
        uint64_t h = static_cast<uint32_t>(v.x);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<uint32_t>(v.y);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<uint32_t>(v.z);
        return static_cast<size_t>(h ^ (h >> 32));
    }
};

} // namespace Coords
