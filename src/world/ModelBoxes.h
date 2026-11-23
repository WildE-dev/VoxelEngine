#pragma once
// Solid blocks drawn smaller than their cell (pipes, shafts, a wheel's hub, a pressure
// gauge, a piston's head) are bumped into, stood on, aimed at and outlined as a few
// boxes roughly matching how they're drawn, not as the whole cell
// (Block::ModelBoxes). A pipe's arms reach out to the neighbours it joins, which only
// the world knows: Joins finds them, as the chunk mesher draws them.
#include <glm/glm.hpp>

#include "world/BlockAccess.h"
#include "world/Power.h"

namespace ModelBoxes {

// The faces (a bit each, Face order) a pipe at `cell` reaches out of; 0 for anything else.
inline unsigned Joins(const BlockAccess& world, const glm::ivec3& cell, const Block& block) {
    if (!block.IsPipe() && !IsValveType(block.type)) return 0;
    const BlockType pipe = IsValveType(block.type) ? ValvePipe(block.type) : block.type; // (a valve joins like its pipe)
    unsigned joined = 0;
    for (int face = 0; face < kFaceCount; ++face) {
        Block other;
        if (!world.GetBlock(cell.x + kFaceOffsets[face][0], cell.y + kFaceOffsets[face][1], cell.z + kFaceOffsets[face][2], other)) continue;
        const bool gauge = (pipe == BlockType::AIR_PIPE || pipe == BlockType::FLUID_PIPE) && !IsValveType(block.type) && other.type == BlockType::PRESSURE_GAUGE &&
                           other.axis % 6 == face;
        if (Power::PipeJoins(pipe, other.type) || gauge) joined |= 1u << face;
    }
    return joined;
}

} // namespace ModelBoxes
