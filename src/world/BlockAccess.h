#pragma once

#include <glm/glm.hpp>

#include "world/Block.h"

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
};
