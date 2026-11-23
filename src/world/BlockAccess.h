#pragma once

#include <glm/glm.hpp>

#include "world/Block.h"
#include "world/Multiblocks.h"

// Read access to blocks, shared by the server's world and the client's copy of it, so
// collision, raycasts and player physics run the same code on both.
class BlockAccess
{
public:
    virtual ~BlockAccess() = default;

    // World coordinates. Returns false if the chunk isn't loaded (or hasn't arrived).
    virtual bool GetBlock(int x, int y, int z, Block& block) const = 0;

    // Whether a point is below the water surface in its cell.
    bool IsUnderwater(const glm::vec3& point) const {
        const glm::ivec3 cell = glm::ivec3(glm::floor(point));
        Block block;
        if (!GetBlock(cell.x, cell.y, cell.z, block) || !block.HasWater()) return false;
        Block above;
        if (GetBlock(cell.x, cell.y + 1, cell.z, above) && above.HasWater()) return true; // full under more water
        return point.y - cell.y < block.WaterLevel() / static_cast<float>(Block::kMaxWaterVolume);
    }

    // Whether a block of this type may be placed at `pos`, as far as the blocks go: the
    // cell holds only air, a plant or water, and isn't inside a formed structure (one of
    // the cells its pattern leaves empty); a door has room for its upper half; seeds
    // and saplings go on dirt or grass. The server decides by this, and the client checks
    // it before showing a placement it predicts, so one the server will refuse isn't
    // shown for a moment and then taken back.
    bool CanPlace(const glm::ivec3& pos, BlockType type) const {
        Block existing;
        if (!GetBlock(pos.x, pos.y, pos.z, existing) || !existing.IsPassable()) return false;
        if (InsideStructure(pos)) return false;
        if (type == BlockType::DOOR) {
            Block upper;
            if (!GetBlock(pos.x, pos.y + 1, pos.z, upper) || !upper.IsPassable() || upper.type == BlockType::DOOR_OPEN) return false;
        }
        if (type == BlockType::CROP || type == BlockType::SAPLING || type == BlockType::PINE_SAPLING) {
            Block soil;
            if (!GetBlock(pos.x, pos.y - 1, pos.z, soil) || !(soil.type == BlockType::DIRT || IsGrassType(soil.type)) || !soil.IsFullBlock()) return false;
        }
        return true;
    }

    // Whether `pos` is one of the empty cells inside a formed structure (its pattern's
    // '.' cells, see Multiblocks::Pattern::open), which are kept clear.
    bool InsideStructure(const glm::ivec3& pos) const {
        // Structures are 3 across and a few high: a core within reach whose pattern covers it.
        for (int dx = -2; dx <= 2; dx++)
            for (int dz = -2; dz <= 2; dz++)
                for (int dy = -5; dy <= 5; dy++) {
                    const glm::ivec3 p = pos + glm::ivec3(dx, dy, dz);
                    Block core;
                    if (!GetBlock(p.x, p.y, p.z, core) || core.type != BlockType::MULTIBLOCK) continue;
                    const glm::ivec3 origin = Multiblocks::OriginOf(p, core);
                    for (const auto& cell : Multiblocks::PatternOf(Multiblocks::KindOf(core)).open)
                        if (Multiblocks::Place(origin, Multiblocks::TurnOf(core), cell) == pos) return true;
                }
        return false;
    }
};
