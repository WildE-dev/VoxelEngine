#include "world/Collision.h"

#include <algorithm>
#include <cmath>

#include "world/BlockAccess.h"
#include "world/ModelBoxes.h"

namespace Collision {

namespace {

constexpr float kEpsilon = 1e-4f;

int Floor(float v) { return static_cast<int>(std::floor(v)); }

// Inclusive range of block coordinates a [lo, hi] interval overlaps. A box that
// exactly touches a block face doesn't count as overlapping it.
void CellRange(float lo, float hi, int& first, int& last) {
    first = Floor(lo + kEpsilon);
    last = Floor(hi - kEpsilon);
}

// An upright slope (natural ground, or a chiselled one): a surface to stand on, which
// doesn't block sideways (MoveHorizontal stops at one rising more than a step).
bool IsUprightSlope(const Block& b) { return b.IsPartial() && !b.IsStepped() && b.ShapeDir() == 0 && b.Collides(); }

// A box overlaps [lo, hi] (touching doesn't count).
bool Overlaps(const AABB& box, const glm::vec3& lo, const glm::vec3& hi) {
    return box.min.x < hi.x - kEpsilon && box.max.x > lo.x + kEpsilon && box.min.y < hi.y - kEpsilon && box.max.y > lo.y + kEpsilon &&
           box.min.z < hi.z - kEpsilon && box.max.z > lo.z + kEpsilon;
}

} // namespace

bool BlockOverlaps(const BlockAccess& world, const glm::ivec3& cell, const Block& block, const AABB& box, bool slopes) {
    if (!block.Collides()) return false;
    const glm::vec3 at(cell);
    glm::vec3 lo[Block::kMaxModelBoxes], hi[Block::kMaxModelBoxes];
    const int count = block.ModelBoxes(ModelBoxes::Joins(world, cell, block), lo, hi);
    if (count >= 0) {
        // Its boxes (a pipe, a shaft, a door's panel), not the whole cell.
        for (int i = 0; i < count; i++)
            if (Overlaps(box, lo[i] + at, hi[i] + at)) return true;
        return false;
    }
    // Quarters and turned shapes block where they actually are; upright slopes only if asked.
    if (block.IsPartial()) return (slopes || !IsUprightSlope(block)) && block.ShapeOverlaps(box.min - at, box.max - at);
    return Overlaps(box, at, at + glm::vec3(1.0f));
}

bool BoxBlocked(const BlockAccess& world, const AABB& box) { return BoxBlocked(world, box, 1e30f); }

bool BoxBlocked(const BlockAccess& world, const AABB& box, float slopesFrom) {
    int x0, x1, y0, y1, z0, z1;
    CellRange(box.min.x, box.max.x, x0, x1);
    CellRange(box.min.y, box.max.y, y0, y1);
    CellRange(box.min.z, box.max.z, z0, z1);

    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) {
                Block block;
                if (!world.GetBlock(x, y, z, block)) return true; // unloaded
                if (BlockOverlaps(world, glm::ivec3(x, y, z), block, box, static_cast<float>(y) >= slopesFrom - kEpsilon)) return true;
            }
    return false;
}

std::optional<float> SlopeHeight(const BlockAccess& world, float x, float z, float yLo, float yHi, bool& unknown) {
    return SlopeHeight(world, AABB{ { x, yLo, z }, { x, yHi, z } }, yLo, yHi, unknown);
}

std::optional<float> SlopeHeight(const BlockAccess& world, const AABB& box, float yLo, float yHi, bool& unknown) {
    // The highest point of any upright slope under the footprint, in each column it covers.
    int x0, x1, z0, z1;
    const bool point = box.min.x == box.max.x && box.min.z == box.max.z;
    if (point) x0 = x1 = Floor(box.min.x), z0 = z1 = Floor(box.min.z);
    else {
        CellRange(box.min.x, box.max.x, x0, x1);
        CellRange(box.min.z, box.max.z, z0, z1);
    }
    std::optional<float> best;
    for (int bx = x0; bx <= x1; bx++)
        for (int bz = z0; bz <= z1; bz++) {
            const float fx0 = box.min.x - bx, fx1 = box.max.x - bx, fz0 = box.min.z - bz, fz1 = box.max.z - bz;
            // Start just below yHi: a box whose top exactly touches a ceiling must not count
            // the ceiling block as part of the range (it would hide the slope underneath).
            for (int y = Floor(yHi - kEpsilon); y >= Floor(yLo); y--) {
                Block block;
                if (!world.GetBlock(bx, y, bz, block)) {
                    unknown = true;
                    return std::nullopt;
                }
                if (block.IsFullBlock()) break; // a full block, not a slope
                if (!IsUprightSlope(block)) continue; // air, a plant, a door, quarters or a turned shape (boxes)
                const float surface = y + block.SurfaceMax(fx0, fx1, fz0, fz1);
                if (surface > yHi) continue; // (above the range: look under it)
                if (surface >= yLo && (!best || surface > *best)) best = surface;
                break;
            }
        }
    return best;
}

std::optional<float> GroundHeight(const BlockAccess& world, const AABB& box, float above, float below, bool& unknown) {
    const float feet = box.min.y;
    const float top = feet + above, bottom = feet - below;
    std::optional<float> best;
    auto consider = [&](float h) { if (h >= bottom && h <= top && (!best || h > *best)) best = h; };

    // The tops of full blocks, quarters and turned shapes under any part of the
    // footprint: scan each column downward and stop at the first one under the feet.
    int x0, x1, z0, z1;
    CellRange(box.min.x, box.max.x, x0, x1);
    CellRange(box.min.z, box.max.z, z0, z1);
    for (int x = x0; x <= x1; x++) {
        for (int z = z0; z <= z1; z++) {
            for (int y = Floor(top - kEpsilon); y >= Floor(bottom) - 1; y--) {
                Block block;
                if (!world.GetBlock(x, y, z, block)) {
                    unknown = true;
                    break;
                }
                if (!block.StoodOn()) continue; // (not solid, or not something to stand on: a door's top)
                {
                    // Drawn smaller than the cell: the tops of its boxes under the footprint
                    // (none there: look on down past it).
                    glm::vec3 lo[Block::kMaxModelBoxes], hi[Block::kMaxModelBoxes];
                    const int count = block.ModelBoxes(ModelBoxes::Joins(world, glm::ivec3(x, y, z), block), lo, hi);
                    if (count >= 0) {
                        float highest = -1.0f;
                        for (int i = 0; i < count; i++)
                            if (box.min.x - x < hi[i].x - kEpsilon && box.max.x - x > lo[i].x + kEpsilon && box.min.z - z < hi[i].z - kEpsilon &&
                                box.max.z - z > lo[i].z + kEpsilon)
                                highest = std::max(highest, hi[i].y);
                        if (highest >= 0.0f) {
                            consider(static_cast<float>(y) + highest);
                            break;
                        }
                        continue;
                    }
                }
                if (!block.IsPartial()) { // (a full block, or anything else solid: its cell)
                    consider(static_cast<float>(y + 1));
                    break;
                }
                if (IsUprightSlope(block)) continue;
                const float shapeTop = block.ShapeTop(box.min.x - x, box.max.x - x, box.min.z - z, box.max.z - z);
                if (shapeTop >= 0.0f) {
                    consider(static_cast<float>(y) + shapeTop);
                    break;
                }
            }
        }
    }

    // Slope surfaces: the highest point of them under the footprint.
    if (auto slope = SlopeHeight(world, box, bottom, top, unknown)) consider(*slope);

    return best;
}

} // namespace Collision
