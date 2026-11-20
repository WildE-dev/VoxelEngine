#pragma once

#include <array>

#include <glm/glm.hpp>

#include "world/Chunk.h"

class TerrainGenerator;

// Surface features: trees, bushes and boulders.
//
// Each kind lives on its own jittered grid (at most one per grid cell, at a hashed
// offset), so placement is deterministic and evenly spaced, and a chunk can generate
// the parts of features rooted in neighbouring columns without loading them.
// Features never hang solid blocks over open air (see TerrainGenerator).
namespace Features {

void Place(const TerrainGenerator& terrain, const glm::ivec3& chunkCoords,
           const std::array<int, Chunk::CHUNK_SIZE * Chunk::CHUNK_SIZE>& surfaceRows,
           std::array<Block, Chunk::VOLUME>& blocks);

// Conservative: true if a feature may cover this column's ground (a trunk, bush or
// boulder). Grass and flowers stay off these columns.
bool MayOccupyColumn(int wx, int wz);

} // namespace Features
