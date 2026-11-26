#pragma once

#include <array>

#include <glm/glm.hpp>

#include "world/Chunk.h"

// Cave carving from 3D noise:
//  - Tunnels ("spaghetti"): where two independent noise fields are both near zero,
//    which traces long winding tubes. Where they reach the surface they make entrances.
//  - Caverns: large open pockets where a third field is high, only well underground.
//
// Noise is sampled on a lattice every kSpacing blocks and trilinearly interpolated,
// which is much cheaper than evaluating it per block and keeps cave walls smooth.
// Single-point queries use exactly the same lattice and interpolation as bulk carving.
namespace Caves {

constexpr int kSpacing = 4;

// Carves caves into generated terrain. surfaceRows is indexed [x * CHUNK_SIZE + z].
void Carve(const glm::ivec3& chunkCoords, const std::array<int, Chunk::CHUNK_SIZE * Chunk::CHUNK_SIZE>& surfaceRows,
           std::array<Block, Chunk::VOLUME>& blocks);

// Whether the block at a world position is carved out, given its column's surface row.
// Columns at or below sea level never get entrances, so seas don't sit over caves.
bool IsCave(int wx, int wy, int wz, int surfaceRow);

// How open the cave field is at a world point (the same lattice and interpolation as
// Carve's): above 0 carved out (exactly where IsCave says so), below 0 rock; between two
// points of opposite sign the cave's wall is about where it crosses 0. Made for one chunk,
// its lattice sampled once for the chunk and the points one block round it.
class Field {
public:
    explicit Field(const glm::ivec3& chunkCoords);
    float Openness(int wx, int wy, int wz, int surfaceRow) const;

private:
    static constexpr int kPoints = Chunk::CHUNK_SIZE / kSpacing + 3;
    glm::ivec3 base{ 0 };                            // the lattice's first point (world)
    std::array<float, kPoints * kPoints * kPoints * 3> lattice{}; // tunnel A, tunnel B, cavern
};

} // namespace Caves
