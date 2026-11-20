#include "world/Raycast.h"

#include <cfloat>
#include <cmath>

#include "world/World.h"

namespace {

// Amanatides & Woo voxel traversal:
// http://www.cse.chalmers.se/edu/year/2010/course/TDA361/grid.pdf
// Calls visit(block, normal) for each voxel the ray passes through until it
// returns false or maxDistance is reached. `direction` must be normalised.
template <typename Visitor>
void WalkVoxels(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, Visitor visit) {
    glm::ivec3 pos(std::floor(origin.x), std::floor(origin.y), std::floor(origin.z));
    glm::ivec3 step;
    glm::vec3 tDelta, tMax;

    for (int axis = 0; axis < 3; axis++) {
        step[axis] = direction[axis] > 0 ? 1 : -1;
        if (direction[axis] == 0.0f) {
            tDelta[axis] = FLT_MAX;
            tMax[axis] = FLT_MAX;
        }
        else {
            tDelta[axis] = std::abs(1.0f / direction[axis]);
            float dist = step[axis] > 0 ? (pos[axis] + 1 - origin[axis]) : (origin[axis] - pos[axis]);
            tMax[axis] = tDelta[axis] * dist;
        }
    }

    glm::ivec3 normal(0);
    float t = 0.0f;
    while (t <= maxDistance) {
        if (!visit(pos, normal)) return;

        // Step along whichever axis reaches its next voxel boundary first.
        int axis = tMax.x < tMax.y ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
        pos[axis] += step[axis];
        t = tMax[axis];
        tMax[axis] += tDelta[axis];
        normal = glm::ivec3(0);
        normal[axis] = -step[axis];
    }
}

} // namespace

std::optional<RaycastHit> Raycast(const World& world, const glm::vec3& origin, const glm::vec3& direction, float maxDistance) {
    std::optional<RaycastHit> hit;
    WalkVoxels(origin, direction, maxDistance, [&](const glm::ivec3& pos, const glm::ivec3& normal) {
        Block block;
        if (world.GetBlock(pos.x, pos.y, pos.z, block) && !block.IsAir()) {
            hit = RaycastHit{ pos, normal };
            return false;
        }
        return true;
    });
    return hit;
}

std::vector<glm::ivec3> TraceBlocks(const World& world, const glm::vec3& origin, const glm::vec3& direction, float maxDistance) {
    std::vector<glm::ivec3> blocks;
    bool hitSolid = false;
    WalkVoxels(origin, direction, maxDistance, [&](const glm::ivec3& pos, const glm::ivec3&) {
        Block block;
        if (world.GetBlock(pos.x, pos.y, pos.z, block)) {
            hitSolid |= !block.IsAir();
            blocks.push_back(pos);
        }
        return true;
    });
    if (!hitSolid) blocks.clear();
    return blocks;
}
