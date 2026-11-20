#include "world/Collision.h"

#include <cmath>

#include "world/World.h"

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

} // namespace

bool BoxBlocked(const World& world, const AABB& box) {
    int x0, x1, y0, y1, z0, z1;
    CellRange(box.min.x, box.max.x, x0, x1);
    CellRange(box.min.y, box.max.y, y0, y1);
    CellRange(box.min.z, box.max.z, z0, z1);

    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) {
                Block block;
                if (!world.GetBlock(x, y, z, block)) return true; // unloaded
                if (block.IsFullBlock()) return true;
            }
    return false;
}

std::optional<float> SlopeHeight(const World& world, float x, float z, float yLo, float yHi, bool& unknown) {
    const int bx = Floor(x), bz = Floor(z);
    const float fx = x - bx, fz = z - bz;
    // Start just below yHi: a box whose top exactly touches a ceiling must not count
    // the ceiling block as part of the range (it would hide the slope underneath).
    for (int y = Floor(yHi - kEpsilon); y >= Floor(yLo); y--) {
        Block block;
        if (!world.GetBlock(bx, y, bz, block)) {
            unknown = true;
            return std::nullopt;
        }
        if (block.IsAir()) continue;
        if (block.IsFullBlock()) return std::nullopt; // a full block, not a slope
        const float surface = y + block.SurfaceHeight(fx, fz);
        if (surface <= yHi && surface >= yLo) return surface;
        if (surface < yLo) return std::nullopt;
    }
    return std::nullopt;
}

std::optional<float> GroundHeight(const World& world, const AABB& box, float above, float below, bool& unknown) {
    const float feet = box.min.y;
    const float top = feet + above, bottom = feet - below;
    std::optional<float> best;
    auto consider = [&](float h) { if (h >= bottom && h <= top && (!best || h > *best)) best = h; };

    // Full block tops under any part of the footprint: scan each column downward
    // and stop at its first full block.
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
                if (block.IsFullBlock()) {
                    consider(static_cast<float>(y + 1));
                    break;
                }
            }
        }
    }

    // Slope surface at the centre of the footprint.
    const float cx = (box.min.x + box.max.x) * 0.5f;
    const float cz = (box.min.z + box.max.z) * 0.5f;
    if (auto slope = SlopeHeight(world, cx, cz, bottom, top, unknown)) consider(*slope);

    return best;
}

} // namespace Collision
