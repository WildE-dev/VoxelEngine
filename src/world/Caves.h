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
bool IsCave(int wx, int wy, int wz, int surfaceRow);

} // namespace Caves
