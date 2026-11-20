#include "world/Raycast.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "world/BlockAccess.h"

namespace {

// Amanatides & Woo voxel traversal:
// http://www.cse.chalmers.se/edu/year/2010/course/TDA361/grid.pdf
// Calls visit(block, normal, tEnter, tExit) for each voxel the ray passes through
// (with the distances along the ray where it enters and leaves it) until it returns
// false or maxDistance is reached. `direction` must be normalised.
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
        // Step along whichever axis reaches its next voxel boundary first.
        int axis = tMax.x < tMax.y ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
        if (!visit(pos, normal, t, std::min(tMax[axis], maxDistance))) return;

        pos[axis] += step[axis];
        t = tMax[axis];
        tMax[axis] += tDelta[axis];
        normal = glm::ivec3(0);
        normal[axis] = -step[axis];
    }
}

// Whether the ray, between tEnter and tExit (inside the block's cell), touches the
// block's actual shape: all of a full block or a plant's cell; a stepped block's four
// quarter-columns; under a slope's surface.
bool HitsShape(const Block& block, const glm::ivec3& cell, const glm::vec3& origin, const glm::vec3& direction, float tEnter, float tExit) {
    if (block.IsFullBlock() || block.IsPlant()) return true;
    const glm::vec3 base(cell);
    if (block.IsStepped()) {
        const int h[2][2] = { { block.edgeData.GetTopY(2), block.edgeData.GetTopY(0) }, { block.edgeData.GetTopY(1), block.edgeData.GetTopY(3) } };
        for (int qx = 0; qx < 2; qx++)
            for (int qz = 0; qz < 2; qz++) {
                if (h[qx][qz] == 0) continue;
                const glm::vec3 lo = base + glm::vec3(qx * 0.5f, 0.0f, qz * 0.5f);
                const glm::vec3 hi = lo + glm::vec3(0.5f, h[qx][qz] / 8.0f, 0.5f);
                float t0 = tEnter, t1 = tExit;
                for (int a = 0; a < 3 && t0 <= t1; a++) {
                    if (std::abs(direction[a]) < 1e-8f) {
                        if (origin[a] < lo[a] || origin[a] > hi[a]) t0 = t1 + 1.0f;
                        continue;
                    }
                    float ta = (lo[a] - origin[a]) / direction[a], tb = (hi[a] - origin[a]) / direction[a];
                    if (ta > tb) std::swap(ta, tb);
                    t0 = std::max(t0, ta);
                    t1 = std::min(t1, tb);
                }
                if (t0 <= t1) return true;
            }
        return false;
    }
    // A slope: sample along the ray through the cell (fine enough for aiming).
    constexpr int kSamples = 24;
    for (int i = 0; i <= kSamples; i++) {
        const glm::vec3 p = origin + direction * (tEnter + (tExit - tEnter) * i / kSamples) - base;
        const float fx = std::clamp(p.x, 0.0f, 1.0f), fz = std::clamp(p.z, 0.0f, 1.0f);
        if (p.y <= block.SurfaceHeight(fx, fz)) return true;
    }
    return false;
}

} // namespace

std::optional<RaycastHit> Raycast(const BlockAccess& world, const glm::vec3& origin, const glm::vec3& direction, float maxDistance) {
    std::optional<RaycastHit> hit;
    WalkVoxels(origin, direction, maxDistance, [&](const glm::ivec3& pos, const glm::ivec3& normal, float tEnter, float tExit) {
        Block block;
        // Rays pass through water (you click the sea floor, not the surface), and
        // through the open part of a partial block (aiming past a slope or a step).
        if (world.GetBlock(pos.x, pos.y, pos.z, block) && !block.IsAir() && !block.IsLiquid() &&
            HitsShape(block, pos, origin, direction, tEnter, tExit)) {
            hit = RaycastHit{ pos, normal };
            return false;
        }
        return true;
    });
    return hit;
}

std::vector<glm::ivec3> TraceBlocks(const BlockAccess& world, const glm::vec3& origin, const glm::vec3& direction, float maxDistance) {
    std::vector<glm::ivec3> blocks;
    bool hitSolid = false;
    WalkVoxels(origin, direction, maxDistance, [&](const glm::ivec3& pos, const glm::ivec3&, float, float) {
        Block block;
        if (world.GetBlock(pos.x, pos.y, pos.z, block)) {
            hitSolid |= !block.IsAir() && !block.IsLiquid();
            blocks.push_back(pos);
        }
        return true;
    });
    if (!hitSolid) blocks.clear();
    return blocks;
}
